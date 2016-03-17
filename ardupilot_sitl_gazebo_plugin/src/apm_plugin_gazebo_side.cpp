/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Gazebo Plugin:  ardupilot_sitl_gazebo_plugin
 * Description: 
 *   Implements plugin's methods related to communication with Gazebo.
 *   e.g. initialization, Gazebo topics subscriptions, callback methods
 */


// FUTURE TODO:
// implement a reset, using for GUI inputs the callback 'on_gazebo_control(...)' with:
//    _msg->has_reset()
//    _msg->reset().has_all()
//    _msg->reset().has_time_only()
//    _msg->reset().has_model_only()


#include "../include/ardupilot_sitl_gazebo_plugin/ardupilot_sitl_gazebo_plugin.h"

namespace gazebo
{

//-------------------------------------------------
//  Initialization methods
//-------------------------------------------------

/*
  Initializes variables related to Gazebo.
  In case of fatal failure, returns 'false'.
 */
bool ArdupilotSitlGazeboPlugin::init_gazebo_side(physics::WorldPtr world, sdf::ElementPtr sdf)
{
    // Saves pointers to the parent world
    _parent_world = world;
    _sdf = sdf;

    // Wait until rover model is inserted
    //ROS_INFO("Waiting for rover model to be inserted...");
    //while (!world->GetModel("rover")){
    //    continue;
    //}
    //ROS_INFO("Rover model inserted!");
    
    // Setup Gazebo node infrastructure

    if (_sdf->HasElement("UAV_MODEL"))
        _modelName = _sdf->Get<std::string>("UAV_MODEL");
    if (_sdf->HasElement("NB_SERVOS_MOTOR_SPEED"))
        _nbMotorSpeed = _sdf->Get<int>("NB_SERVOS_MOTOR_SPEED");
    ROS_INFO("Model name:      %s", _modelName.c_str());
    ROS_INFO("Nb motor servos: %d", _nbMotorSpeed);

    

    // 'transport' is the communication library of Gazebo. It handles publishers
    // and subscribers.
    transport::NodePtr node(new transport::Node());

    // Initialize the node with the world name
    node->Init(_parent_world->GetName());
    
    _parent_world->SetPaused(true);
    
    // Create a publisher on the ~/physics topic
    transport::PublisherPtr physicsPub = node->Advertise<msgs::Physics>("~/physics");
    msgs::Physics physicsMsg;
    physicsMsg.set_type(msgs::Physics::ODE);

    // Set the step time: 2.5 ms to achieve the 400 Hz required by ArduPilot on Pixhawk
    // TODO: pass it to parametric
    physicsMsg.set_max_step_size(STEP_SIZE_FOR_ARDUPILOT);
    physicsPub->Publish(physicsMsg);
    
    _controlSub = node->Subscribe("~/world_control", &ArdupilotSitlGazeboPlugin::on_gazebo_control, this);
    
    _modelInfoSub = node->Subscribe("~/model/info", &ArdupilotSitlGazeboPlugin::on_gazebo_modelInfo, this);
    
    //this->newFrameConnection = this->camera->ConnectNewImageFrame(
    //  boost::bind(&CameraPlugin::OnNewFrame, this, _1, _2, _3, _4, _5));

    _updateConnection = event::Events::ConnectWorldUpdateEnd(
          boost::bind(&ArdupilotSitlGazeboPlugin::on_gazebo_update, this));
    // Or we could also use 'ConnectWorldUpdateBegin'
    // For a list of all available connection events, see: Gazebo-X.X/gazebo/common/Events.hh 
    
    return true;
}


////////////////////////////////////////////////////////////////////////////////
// function that extracts the radius of a cylinder or sphere collision shape
// the function returns zero otherwise
double ArdupilotSitlGazeboPlugin::get_collision_radius(physics::CollisionPtr _coll)
{
  if (!_coll || !(_coll->GetShape()))
    return 0;
  if (_coll->GetShape()->HasType(gazebo::physics::Base::CYLINDER_SHAPE))
  {
    physics::CylinderShape *cyl =
        static_cast<physics::CylinderShape*>(_coll->GetShape().get());
    return cyl->GetRadius();
  }
  else if (_coll->GetShape()->HasType(physics::Base::SPHERE_SHAPE))
  {
    physics::SphereShape *sph =
        static_cast<physics::SphereShape*>(_coll->GetShape().get());
    return sph->GetRadius();
  }
  return 0;
}

    
//-------------------------------------------------
//  Gazebo communication
//-------------------------------------------------

/*
  Advances the simulation by 1 step
 */
void ArdupilotSitlGazeboPlugin::step_gazebo_sim()
{
    // The simulation must be in Pause mode for the Step function to work.
    // This ensures that Ardupilot does not miss a single step of the physics solver.
    // Unfortunately, it breaks the Gazebo system of Real Time clock and Factor,
    // as well as the functionnality of the Pause & Step GUI buttons.
    // The functionnality of the Pause GUI button is emulated within 'on_gazebo_control()'.
    _parent_world->Step(1);
}

/*
  Callback from gazebo after each simulation step
  (thus after each call to 'step_gazebo_sim()')
 */
void ArdupilotSitlGazeboPlugin::on_gazebo_update()
{
    // This method is executed independently from the main loop thread.
    // Beware of access to shared variables memory. USe mutexes if required.
    
    // Get the simulation time
    gazebo::common::Time gz_time_now = _parent_world->GetSimTime();
    // Converts it to seconds
    _fdm.timestamp = gz_time_now.sec + gz_time_now.nsec * 1e-9;

    if (!_timeMsgAlreadyDisplayed) {
        // (It seems) The displayed value is only updated after the first iteration
        ROS_INFO( PLUGIN_LOG_PREPEND "Simulation step size is = %f", _parent_world->GetPhysicsEngine()->GetMaxStepSize());
        _timeMsgAlreadyDisplayed = true;
    }
}
    
/*
  Emulates the Pause GUI button functionnality.
  Shortcomings: The GUI button does not change shape between Play/Resume
 */
void ArdupilotSitlGazeboPlugin::on_gazebo_control(ConstWorldControlPtr &_msg)
{
    // This method is executed independently from the main loop thread.
    // Beware of access to shared variables memory. Use mutexes if required.
    
    if (_msg->has_pause()) {
        // The plugin's running principle, based on explicit calls to Gazebo's step() method,
        // requires Gazebo to be in a continuous pause state.
        // Indeed, Gazebo must permanently remain in pause, until the next call to step().
        // Therefore the state of the GUI play/pause button is not reliable, and the
        // actual play/pause state must be handled here, with '_isSimPaused'.
        
        _isSimPaused = !_isSimPaused;
        
        if (_isSimPaused) {
            _parent_world->SetPaused(true);
            ROS_INFO( PLUGIN_LOG_PREPEND "Simulation is now paused");
        } else {
            ROS_INFO( PLUGIN_LOG_PREPEND "Resuming simulation");
        }
    }
}

void ArdupilotSitlGazeboPlugin::on_rover_model_loaded(){

    _rover_model = _parent_world->GetModel("rover");

    //ROVER STUFF
    this->wheelRadius = 0.1;
    this->flWheelRadius = 0.1;
    this->frWheelRadius = 0.1;
    this->blWheelRadius = 0.1;
    this->brWheelRadius = 0.1;
    this->steeredWheelForce = 5000;

    this->frontTorque = 0;
    this->backTorque = 0;
    this->tireAngleRange = 0;
    this->maxSpeed = 0;
    this->maxSteer = 0;
    this->aeroLoad = 0;

    this->node.reset(new transport::Node());
    this->node->Init(_parent_world->GetName());

    ROS_INFO("Searching joints...");
    this->flWheelJoint = _rover_model->GetJoint("front_left_wheel_joint");
    if (!this->flWheelJoint)
        gzthrow("could not find front left wheel joint\n");

    ROS_INFO("Front left joint found");

    this->frWheelJoint = _rover_model->GetJoint("front_right_wheel_joint");
    if (!this->frWheelJoint)
        gzthrow("could not find front right wheel joint\n");

    this->blWheelJoint = _rover_model->GetJoint("rear_left_wheel_joint");
    if (!this->blWheelJoint)
        gzthrow("could not find back left wheel joint\n");

    this->brWheelJoint = _rover_model->GetJoint("rear_right_wheel_joint");
    if (!this->brWheelJoint)
        gzthrow("could not find back right wheel joint\n");

    this->frWheelSteeringJoint = _rover_model->GetJoint("front_left_steering_joint");
    if (!this->frWheelSteeringJoint)
        gzthrow("could not find front right steering joint\n");

    this->flWheelSteeringJoint = _rover_model->GetJoint("front_right_steering_joint");
    if (!this->flWheelSteeringJoint)
        gzthrow("could not find front left steering joint\n");

    // get some vehicle parameters
    std::string paramName;
    double paramDefault;

    paramName = "front_torque";
    paramDefault = 0;
    if (_sdf->HasElement(paramName))
        this->frontTorque = _sdf->Get<double>(paramName);
    else
        this->frontTorque = paramDefault;

    paramName = "back_torque";
    paramDefault = 2000;
    if (_sdf->HasElement(paramName))
        this->backTorque = _sdf->Get<double>(paramName);
    else
        this->backTorque = paramDefault;

    paramName = "max_speed";
    paramDefault = 10;
    if (_sdf->HasElement(paramName))
        this->maxSpeed = _sdf->Get<double>(paramName);
    else
        this->maxSpeed = paramDefault;

    paramName = "max_steer";
    paramDefault = 0.6;
    if (_sdf->HasElement(paramName))
        this->maxSteer = _sdf->Get<double>(paramName);
    else
        this->maxSteer = paramDefault;

    paramName = "aero_load";
    paramDefault = 0.1;
    if (_sdf->HasElement(paramName))
        this->aeroLoad = _sdf->Get<double>(paramName);
    else
        this->aeroLoad = paramDefault;

    // Simulate braking using joint stops with stop_erp = 0
    this->flWheelJoint->SetHighStop(0, 0);
    this->frWheelJoint->SetHighStop(0, 0);
    this->blWheelJoint->SetHighStop(0, 0);
    this->brWheelJoint->SetHighStop(0, 0);

    this->flWheelJoint->SetLowStop(0, 0);
    this->frWheelJoint->SetLowStop(0, 0);
    this->blWheelJoint->SetLowStop(0, 0);
    this->brWheelJoint->SetLowStop(0, 0);

    // stop_erp == 0 means no position correction torques will act
    this->flWheelJoint->SetParam("stop_erp", 0, 0.0);
    this->frWheelJoint->SetParam("stop_erp", 0, 0.0);
    this->blWheelJoint->SetParam("stop_erp", 0, 0.0);
    this->brWheelJoint->SetParam("stop_erp", 0, 0.0);

    // stop_cfm == 10 means the joints will initially have small damping
    this->flWheelJoint->SetParam("stop_cfm", 0, 10.0);
    this->frWheelJoint->SetParam("stop_cfm", 0, 10.0);
    this->blWheelJoint->SetParam("stop_cfm", 0, 10.0);
    this->brWheelJoint->SetParam("stop_cfm", 0, 10.0);

    // Update wheel radius for each wheel from SDF collision objects
    //  assumes that wheel link is child of joint (and not parent of joint)
    //  assumes that wheel link has only one collision
    unsigned int id = 0;
    this->flWheelRadius = ArdupilotSitlGazeboPlugin::get_collision_radius(
                      this->flWheelJoint->GetChild()->GetCollision(id));
    this->frWheelRadius = ArdupilotSitlGazeboPlugin::get_collision_radius(
                      this->frWheelJoint->GetChild()->GetCollision(id));
    this->blWheelRadius = ArdupilotSitlGazeboPlugin::get_collision_radius(
                      this->blWheelJoint->GetChild()->GetCollision(id));
    this->brWheelRadius = ArdupilotSitlGazeboPlugin::get_collision_radius(
                      this->brWheelJoint->GetChild()->GetCollision(id));


}

/*
  Callback method when a new model is added on Gazebo.
  Used to detect the end of asynchronous model loading.
 */
void ArdupilotSitlGazeboPlugin::on_gazebo_modelInfo(ConstModelPtr &_msg)
{
    // This method is executed independently from the main loop thread.
    // Beware of access to shared variables memory. Use mutexes if required.
    
    ROS_INFO( "NEW MODEL : name %s", _msg->name().c_str());
    
    if (!_msg->name().compare("rover")) {
        ROS_INFO("There is Rover !");
        on_rover_model_loaded();
    }
}

} // end of "namespace gazebo"
