#include <rclcpp/rclcpp.hpp>
#include <geometry_msgs/msg/twist.hpp>
#include <sensor_msgs/msg/joint_state.hpp>
#include <thread>
#include <mutex>
#include <map>
#include <string>
#include <vector>
#include <cstring>
#include <unistd.h>
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/document.h"
#include "rapidjson/writer.h"       
#include "rapidjson/stringbuffer.h" 
#include <fcntl.h>
#include <signal.h>

extern "C" {
  #include <ziti/ziti.h>
  #include <ziti/zitilib.h>
  #undef list
  #undef map
}

// 1. FORWARD DECLARATIONS
class ZitiBridge; 
std::shared_ptr<ZitiBridge> global_node_ptr = nullptr;
void signal_handler(int signum);
using namespace std::chrono_literals;

// 2. CLASS DEFINITION
class ZitiBridge : public rclcpp::Node {
public:
    ZitiBridge() : Node("ziti_bridge_node") {
        const char* home = getenv("HOME");
        std::string default_path = std::string(home) + "/identity.json";
        this->declare_parameter("ziti_context_path", default_path);
        this->declare_parameter("ziti_identity_name", "robot1");
        this->declare_parameter("ziti_service_name", "cmd,svc");
        ziti_context_path_ = this->get_parameter("ziti_context_path").as_string();
        ziti_id_name_ = this->get_parameter("ziti_identity_name").as_string();
        ziti_service_name_ = this->get_parameter("ziti_service_name").as_string();
        init_timer_ = this->create_wall_timer(1s, [this]() { this->start_bridge(); });
    }

    // Method to start the Ziti bridge after the context is loaded
    void start_bridge() {
        // 1. Check if we are ready. If not, don't cancel the timer!
        if (ztx_handle_ == 0) {
            RCLCPP_WARN(this->get_logger(), "Ziti Context not yet synchronized. Retrying in 1s...");
            return; 
        }
        srv_cmd_ = Ziti_socket(SOCK_STREAM);
        if (Ziti_bind(srv_cmd_, ztx_handle_, ziti_service_name_.c_str(), ziti_id_name_.c_str()) == 0) {
            Ziti_listen(srv_cmd_, 10);
            RCLCPP_INFO(this->get_logger(), "ZITI BIND SUCCESS: [%s] hosting [%s]", ziti_id_name_.c_str(), ziti_service_name_.c_str());
            init_timer_->cancel();
            ziti_thread_ = std::thread(&ZitiBridge::run_ziti_cmd_loop, this);
        } else {
            RCLCPP_ERROR(this->get_logger(), "ZITI BIND FAILED for identity [%s] hosting [%s]", ziti_id_name_.c_str(), ziti_service_name_.c_str());
            if(srv_cmd_ >= 0){
                close(srv_cmd_);
                srv_cmd_ = -1;
            }
        }
    }
    
    // Method to set the Ziti context after loading
    void set_ztx(ziti_handle_t ztx) {
        this->ztx_handle_ = ztx;
        RCLCPP_INFO(this->get_logger(), "Node handle synchronized with OpenZiti context.");
    }

    std::string get_ziti_context_path() {
        return ziti_context_path_;
    }

    // Public method called by the signal handler
    void stop_all_robots() {
        auto stop_msg = geometry_msgs::msg::Twist();
        std::lock_guard<std::mutex> lock(data_mutex_);
        for (auto const& [topic, pub] : pubs_) {
            pub->publish(stop_msg);
            RCLCPP_INFO(this->get_logger(), "Sent Shutdown STOP to %s", topic.c_str());
        }
    }

    ~ZitiBridge() {
        stop_all_robots();
        if (ziti_thread_.joinable()) ziti_thread_.join();
    }

    std::string process_command(char* json_raw) {
        rapidjson::Document d;
        if (d.Parse(json_raw).HasParseError()) return "";
        if (d.HasMember("ns") && d.HasMember("topic")) {
            std::string ns = d["ns"].GetString();
            std::string topic = d["topic"].GetString();
            std::string full_topic = "/" + ns + "/" + topic;

            std::lock_guard<std::mutex> lock(data_mutex_);
            if (pubs_.find(full_topic) == pubs_.end()) {
                pubs_[full_topic] = this->create_publisher<geometry_msgs::msg::Twist>(full_topic, 10);
            }
            if (subs_.find(ns) == subs_.end()) {
                std::string sub_topic = "/" + ns + "/joint_states";
                auto sensor_qos = rclcpp::SensorDataQoS();
                subs_[ns] = this->create_subscription<sensor_msgs::msg::JointState>(
                    sub_topic, sensor_qos,
                    [this, ns](const sensor_msgs::msg::JointState::SharedPtr msg) {
                        if (!msg->position.empty()) {
                            std::lock_guard<std::mutex> lock_cb(this->data_mutex_);
                            this->robot_positions_[ns] = msg->position[0];
                        }
                    });
            }
            auto twist_msg = geometry_msgs::msg::Twist();
            if (d.HasMember("lx") && d["lx"].IsNumber()) twist_msg.linear.x = d["lx"].GetDouble();
            if (d.HasMember("az") && d["az"].IsNumber()) twist_msg.angular.z = d["az"].GetDouble();
            pubs_[full_topic]->publish(twist_msg);
            return ns;
        }
        return "";
    }

    void run_ziti_cmd_loop() {
    RCLCPP_INFO(this->get_logger(), "Ziti Bridge is READY for multiple controllers.");
    
    while (rclcpp::ok()) {
            char caller[128];
            // This blocks until a robot controller connects
            ziti_socket_t clt = Ziti_accept(srv_cmd_, caller, sizeof(caller));
            
            // If we shut down while waiting for a connection
            if (!rclcpp::ok()) {
                if (clt >= 0) close(clt);
                break;
            }

            if (clt >= 0) {
                std::string caller_id(caller);
                
                // THE RESTORED LOG:
                RCLCPP_INFO(this->get_logger(), ">>> NEW CONTROLLER SESSION: [%s]", caller_id.c_str());

                std::thread([this, clt, caller_id]() {
                    this->handle_session(clt, caller_id);
                }).detach();
            }
        }
    }

    void handle_session(ziti_socket_t clt, std::string caller_id) {
        char buf[2048];
        std::string last_active_ns = "";

        RCLCPP_INFO(this->get_logger(), "Session started for: %s", caller_id.c_str());

        while (rclcpp::ok()) {
            // Standard blocking read - Ziti's SDK handles the 'wait' internally
            // This is the most stable way to prevent "Broken Pipe" crashes
            ssize_t n = read(clt, buf, sizeof(buf) - 1);
            
            if (n > 0) {
                buf[n] = '\0';
                last_active_ns = process_command(buf);

                // Fetch Telemetry
                double current_pos = 0.0;
                if (!last_active_ns.empty()) {
                    std::lock_guard<std::mutex> lock(data_mutex_);
                    if (robot_positions_.count(last_active_ns)) {
                        current_pos = robot_positions_[last_active_ns];
                    }
                }

                // Construct feedback
                rapidjson::Document d;
                d.SetObject();
                rapidjson::Document::AllocatorType& allocator = d.GetAllocator();

                // Match the namespace key
                d.AddMember("ns", rapidjson::Value(last_active_ns.c_str(), allocator).Move(), allocator);

                // Use 'joint_states' as the object key
                rapidjson::Value js_obj(rapidjson::kObjectType);
                js_obj.AddMember("position", current_pos, allocator); // Single float or double

                d.AddMember("joint_states", js_obj, allocator);

                // Serialize
                rapidjson::StringBuffer buffer;
                rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
                d.Accept(writer);

                std::string feedback = std::string(buffer.GetString()) + "\n";
                
                //
                if (write(clt, feedback.c_str(), feedback.length()) < 0) {
                    RCLCPP_ERROR(this->get_logger(), "Write failed for %s. Closing session.", caller_id.c_str());
                    break;
                }
            } else {
                // n <= 0 means the Ziti SDK has closed the session or timed it out internally
                if (n == 0) {
                    RCLCPP_INFO(this->get_logger(), "Controller %s disconnected gracefully.", caller_id.c_str());
                } else {
                    RCLCPP_WARN(this->get_logger(), "Socket closed for %s (n=%ld).", caller_id.c_str(), n);
                }
                break; 
            }
        }

        // Safety: If the loop breaks, the controller is gone. Send a STOP to be safe.
        if (!last_active_ns.empty()) {
            auto stop_msg = geometry_msgs::msg::Twist();
            std::lock_guard<std::mutex> lock(data_mutex_);
            std::string cmd_topic = "/" + last_active_ns + "/cmd_vel";
            if (pubs_.count(cmd_topic)) {
                pubs_[cmd_topic]->publish(stop_msg);
                RCLCPP_INFO(this->get_logger(), "Safety Stop sent to %s", last_active_ns.c_str());
            }
        }

        close(clt);
    }

private:
    ziti_handle_t ztx_handle_ = 0;
    ziti_socket_t srv_cmd_ = -1;
    std::string ziti_id_name_;
    std::string ziti_service_name_;
    std::string ziti_context_path_;
    std::thread ziti_thread_;
    rclcpp::TimerBase::SharedPtr init_timer_;
    std::mutex data_mutex_;
    std::map<std::string, rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr> pubs_;
    std::map<std::string, rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr> subs_;
    std::map<std::string, double> robot_positions_;
};

void signal_handler(int signum) {
    // Avoid re-entry if the user mashes Ctrl+C
    static bool shutting_down = false;
    if (shutting_down) exit(signum); 
    shutting_down = true;

    if (global_node_ptr) {
        RCLCPP_INFO(rclcpp::get_logger("rclcpp"), "Interrupt (%d) - Stopping Fleet and Exiting...", signum);
        global_node_ptr->stop_all_robots();
    }

    // 1. Tell ROS to stop
    rclcpp::shutdown();

    // 2. Force the process to exit
    // This breaks the thread stuck in Ziti_accept()
    RCLCPP_INFO(rclcpp::get_logger("rclcpp"), "Shutdown complete.");
    exit(signum); 
}

// 4. MAIN
int main(int argc, char **argv) {
    // Register handlers before ROS init
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    rclcpp::init(argc, argv);
    Ziti_lib_init();

    ziti_handle_t global_ztx = 0;
    global_node_ptr = std::make_shared<ZitiBridge>();

    if (Ziti_load_context(&global_ztx, global_node_ptr->get_ziti_context_path().c_str()) != ZITI_OK) {
        fprintf(stderr, "Failed to load Ziti Context from %s\n", global_node_ptr->get_ziti_context_path().c_str());
        return -1;
    }else {
        RCLCPP_INFO(rclcpp::get_logger("rclcpp"), "Ziti Context loaded successfully from %s", global_node_ptr->get_ziti_context_path().c_str());
    }

    // Assign to the global pointer so the signal handler can use it
    global_node_ptr->set_ztx(global_ztx);
    rclcpp::spin(global_node_ptr);

    Ziti_lib_shutdown();
    rclcpp::shutdown();
    return 0;
}