import rclpy
from rclpy.node import Node
from rclpy.callback_groups import ReentrantCallbackGroup
from rclpy.executors import MultiThreadedExecutor
from sensor_msgs.msg import JointState
from geometry_msgs.msg import Twist
from rclpy.qos import QoSProfile, ReliabilityPolicy
import argparse

class DemoRobot(Node):
    def __init__(self, namespace):
        super().__init__('demo_robot_driver', namespace=namespace)
        
        # Use a ReentrantCallbackGroup so the timer can run while 
        # the subscription is busy or vice versa
        self.group = ReentrantCallbackGroup()

        qos = QoSProfile(reliability=ReliabilityPolicy.BEST_EFFORT, depth=10)

        self.pub = self.create_publisher(JointState, 'joint_states', qos)
        
        # Assign the callback group to the sub and timer
        self.sub = self.create_subscription(
            Twist, 'cmd_vel', self.cmd_callback, 10, callback_group=self.group)

        self.timer = self.create_timer(
            0.1, self.timer_callback, callback_group=self.group)

        # --- INITIALIZE STATE VARIABLES ---
        self.angle = 0.0
        self.linear_velocity = 0.0
        self.angular_velocity = 0.0  # <--- THIS WAS MISSING
        self.last_command_time = self.get_clock().now()

    def cmd_callback(self, msg):
        # Capture BOTH axes so 'A' and 'D' work
        self.linear_velocity = msg.linear.x
        self.angular_velocity = msg.angular.z
        self.last_command_time = self.get_clock().now()
        
        # Log both so we can verify the bridge is sending both
        self.get_logger().info(f"Recv: LX: {self.linear_velocity:.2f}, AZ: {self.angular_velocity:.2f}")

    def timer_callback(self):
        now = self.get_clock().now()
        elapsed_duration = now - self.last_command_time
        
        # 1. WATCHDOG LOGIC (Safety Layer)
        # This only sets the TARGET velocities to 0 if the link is dead
        if elapsed_duration.nanoseconds > 500_000_000:
            if getattr(self, 'link_active', True):
                self.get_logger().error("WATCHDOG: Link Lost!")
                self.link_active = False
            
            self.linear_velocity = 0.0
            self.angular_velocity = 0.0
        else:
            self.link_active = True

        # 2. PHYSICS LOGIC (Simulation Layer)
        # This must run every cycle (0.1s) regardless of whether the link is active
        # If velocities are 0, angle won't change, but the message still publishes
        self.angle += (self.linear_velocity + self.angular_velocity) * 0.1

        # 3. REPORTING (Telemetry Layer)
        msg = JointState()
        msg.header.stamp = now.to_msg()
        msg.name = ['front_left_wheel', 'front_right_wheel']
        msg.position = [self.angle, self.angle]
        self.pub.publish(msg)

def main():
    parser = argparse.ArgumentParser(description="ros2 robot sim")
    parser.add_argument("--namespace", type=str, required=True, help="ros2 name space")
    args = parser.parse_args()
    rclpy.init()
    node = DemoRobot(args.namespace)
    
    # Use MultiThreadedExecutor so the watchdog timer isn't blocked by message bursts
    executor = MultiThreadedExecutor()
    executor.add_node(node)
    
    try:
        executor.spin()
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        rclpy.shutdown()

if __name__ == '__main__':
    main()
