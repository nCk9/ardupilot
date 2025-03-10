#include "tiltrotor.h"
#include "Plane.h"

#if HAL_QUADPLANE_ENABLED
const AP_Param::GroupInfo Tiltrotor::var_info[] = {

    // @Param: ENABLE
    // @DisplayName: Enable Tiltrotor functionality
    // @Values: 0:Disable, 1:Enable
    // @Description: This enables Tiltrotor functionality
    // @User: Standard
    // @RebootRequired: True
    AP_GROUPINFO_FLAGS("ENABLE", 1, Tiltrotor, enable, 0, AP_PARAM_FLAG_ENABLE),

    // @Param: MASK
    // @DisplayName: Tiltrotor mask
    // @Description: This is a bitmask of motors that are tiltable in a tiltrotor (or tiltwing). The mask is in terms of the standard motor order for the frame type.
    // @User: Standard
    AP_GROUPINFO("MASK", 2, Tiltrotor, tilt_mask, 0),

    // @Param: RATE_UP
    // @DisplayName: Tiltrotor upwards tilt rate
    // @Description: This is the maximum speed at which the motor angle will change for a tiltrotor when moving from forward flight to hover
    // @Units: deg/s
    // @Increment: 1
    // @Range: 10 300
    // @User: Standard
    AP_GROUPINFO("RATE_UP", 3, Tiltrotor, max_rate_up_dps, 40),

    // @Param: MAX
    // @DisplayName: Tiltrotor maximum VTOL angle
    // @Description: This is the maximum angle of the tiltable motors at which multicopter control will be enabled. Beyond this angle the plane will fly solely as a fixed wing aircraft and the motors will tilt to their maximum angle at the TILT_RATE
    // @Units: deg
    // @Increment: 1
    // @Range: 20 80
    // @User: Standard
    AP_GROUPINFO("MAX", 4, Tiltrotor, max_angle_deg, 45),

    // @Param: TYPE
    // @DisplayName: Tiltrotor type
    // @Description: This is the type of tiltrotor when TILT_MASK is non-zero. A continuous tiltrotor can tilt the rotors to any angle on demand. A binary tiltrotor assumes a retract style servo where the servo is either fully forward or fully up. In both cases the servo can't move faster than Q_TILT_RATE. A vectored yaw tiltrotor will use the tilt of the motors to control yaw in hover, Bicopter tiltrottor must use the tailsitter frame class (10)
    // @Values: 0:Continuous,1:Binary,2:VectoredYaw,3:Bicopter
    AP_GROUPINFO("TYPE", 5, Tiltrotor, type, TILT_TYPE_CONTINUOUS),

    // @Param: RATE_DN
    // @DisplayName: Tiltrotor downwards tilt rate
    // @Description: This is the maximum speed at which the motor angle will change for a tiltrotor when moving from hover to forward flight. When this is zero the Q_TILT_RATE_UP value is used.
    // @Units: deg/s
    // @Increment: 1
    // @Range: 10 300
    // @User: Standard
    AP_GROUPINFO("RATE_DN", 6, Tiltrotor, max_rate_down_dps, 0),

    // @Param: YAW_ANGLE
    // @DisplayName: Tilt minimum angle for vectored yaw
    // @Description: This is the angle of the tilt servos when in VTOL mode and at minimum output. This needs to be set for Q_TILT_TYPE=3 to enable vectored control for yaw of tricopter tilt quadplanes. This is also used to limit the forwards travel of bicopter tilts when in VTOL modes
    // @Range: 0 30
    AP_GROUPINFO("YAW_ANGLE", 7, Tiltrotor, tilt_yaw_angle, 0),

    // @Param: FIX_ANGLE
    // @DisplayName: Fixed wing tiltrotor angle
    // @Description: This is the angle the motors tilt down when at maximum output for forward flight. Set this to a non-zero value to enable vectoring for roll/pitch in forward flight on tilt-vectored aircraft
    // @Units: deg
    // @Range: 0 30
    // @User: Standard
    AP_GROUPINFO("FIX_ANGLE", 8, Tiltrotor, fixed_angle, 0),

    // @Param: FIX_GAIN
    // @DisplayName: Fixed wing tiltrotor gain
    // @Description: This is the gain for use of tilting motors in fixed wing flight for tilt vectored quadplanes
    // @Range: 0 1
    // @User: Standard
    AP_GROUPINFO("FIX_GAIN", 9, Tiltrotor, fixed_gain, 0),

    AP_GROUPEND
};

/*
  control code for tiltrotors and tiltwings. Enabled by setting
  Q_TILT_MASK to a non-zero value
 */

Tiltrotor::Tiltrotor(QuadPlane& _quadplane, AP_MotorsMulticopter*& _motors):quadplane(_quadplane),motors(_motors)
{
    AP_Param::setup_object_defaults(this, var_info);
}

void Tiltrotor::setup()
{

    if (!enable.configured() && ((tilt_mask != 0) || (type == TILT_TYPE_BICOPTER))) {
        enable.set_and_save(1);
    }

    if (enable <= 0) {
        return;
    }

    _is_vectored = tilt_mask != 0 && type == TILT_TYPE_VECTORED_YAW;

    if (quadplane.motors_var_info == AP_MotorsMatrix::var_info && _is_vectored) {
        // we will be using vectoring for yaw
        motors->disable_yaw_torque();
    }

    if (tilt_mask != 0) {
        // setup tilt compensation
        motors->set_thrust_compensation_callback(FUNCTOR_BIND_MEMBER(&Tiltrotor::tilt_compensate, void, float *, uint8_t));
        if (type == TILT_TYPE_VECTORED_YAW) {
            // setup tilt servos for vectored yaw
            SRV_Channels::set_range(SRV_Channel::k_tiltMotorLeft,  1000);
            SRV_Channels::set_range(SRV_Channel::k_tiltMotorRight, 1000);
            SRV_Channels::set_range(SRV_Channel::k_tiltMotorRear,  1000);
            SRV_Channels::set_range(SRV_Channel::k_tiltMotorRearLeft, 1000);
            SRV_Channels::set_range(SRV_Channel::k_tiltMotorRearRight, 1000);
        }
    }

    transition = new Tiltrotor_Transition(quadplane, motors, *this);
    if (!transition) {
        AP_BoardConfig::allocation_error("tiltrotor transition");
    }
    quadplane.transition = transition;

    setup_complete = true;
}

/*
  calculate maximum tilt change as a proportion from 0 to 1 of tilt
 */
float Tiltrotor::tilt_max_change(bool up) const
{
    float rate;
    if (up || max_rate_down_dps <= 0) {
        rate = max_rate_up_dps;
    } else {
        rate = max_rate_down_dps;
    }
    if (type != TILT_TYPE_BINARY && !up) {
        bool fast_tilt = false;
        if (plane.control_mode == &plane.mode_manual) {
            fast_tilt = true;
        }
        if (hal.util->get_soft_armed() && !quadplane.in_vtol_mode() && !quadplane.assisted_flight) {
            fast_tilt = true;
        }
        if (fast_tilt) {
            // allow a minimum of 90 DPS in manual or if we are not
            // stabilising, to give fast control
            rate = MAX(rate, 90);
        }
    }
    return rate * plane.G_Dt / 90.0f;
}

/*
  output a slew limited tiltrotor angle. tilt is from 0 to 1
 */
void Tiltrotor::slew(float newtilt)
{
    float max_change = tilt_max_change(newtilt<current_tilt);
    current_tilt = constrain_float(newtilt, current_tilt-max_change, current_tilt+max_change);

    // translate to 0..1000 range and output
    SRV_Channels::set_output_scaled(SRV_Channel::k_motor_tilt, 1000 * current_tilt);
}

/*
  update motor tilt for continuous tilt servos
 */
void Tiltrotor::continuous_update(void)
{
    // default to inactive
    _motors_active = false;

    // the maximum rate of throttle change
    float max_change;

    if (!quadplane.in_vtol_mode() && (!hal.util->get_soft_armed() || !quadplane.assisted_flight)) {
        // we are in pure fixed wing mode. Move the tiltable motors all the way forward and run them as
        // a forward motor
        slew(1);

        max_change = tilt_max_change(false);

        float new_throttle = constrain_float(SRV_Channels::get_output_scaled(SRV_Channel::k_throttle)*0.01, 0, 1);
        if (current_tilt < 1) {
            current_throttle = constrain_float(new_throttle,
                                                    current_throttle-max_change,
                                                    current_throttle+max_change);
        } else {
            current_throttle = new_throttle;
        }
        if (!hal.util->get_soft_armed()) {
            current_throttle = 0;
        } else {
            // prevent motor shutdown
            _motors_active = true;
        }
        if (!quadplane.motor_test.running) {
            // the motors are all the way forward, start using them for fwd thrust
            uint8_t mask = is_zero(current_throttle)?0:(uint8_t)tilt_mask.get();
            motors->output_motor_mask(current_throttle, mask, plane.rudder_dt);
        }
        return;
    }

    // remember the throttle level we're using for VTOL flight
    float motors_throttle = motors->get_throttle();
    max_change = tilt_max_change(motors_throttle<current_throttle);
    current_throttle = constrain_float(motors_throttle,
                                            current_throttle-max_change,
                                            current_throttle+max_change);

    /*
      we are in a VTOL mode. We need to work out how much tilt is
      needed. There are 4 strategies we will use:

      1) without manual forward throttle control, the angle will be set to zero
         in QAUTOTUNE QACRO, QSTABILIZE and QHOVER. This
         enables these modes to be used as a safe recovery mode.

      2) with manual forward throttle control we will set the angle based on
         the demanded forward throttle via RC input.

      3) in fixed wing assisted flight or velocity controlled modes we
         will set the angle based on the demanded forward throttle,
         with a maximum tilt given by Q_TILT_MAX. This relies on
         Q_VFWD_GAIN being set.

      4) if we are in TRANSITION_TIMER mode then we are transitioning
         to forward flight and should put the rotors all the way forward
    */

#if QAUTOTUNE_ENABLED
    if (plane.control_mode == &plane.mode_qautotune) {
        slew(0);
        return;
    }
#endif

    // if not in assisted flight and in QACRO, QSTABILIZE or QHOVER mode
    if (!quadplane.assisted_flight &&
        (plane.control_mode == &plane.mode_qacro ||
         plane.control_mode == &plane.mode_qstabilize ||
         plane.control_mode == &plane.mode_qhover)) {
        if (quadplane.rc_fwd_thr_ch == nullptr) {
            // no manual throttle control, set angle to zero
            slew(0);
        } else {
            // manual control of forward throttle
            float settilt = .01f * quadplane.forward_throttle_pct();
            slew(settilt);
        }
        return;
    }

    if (quadplane.assisted_flight &&
        transition->transition_state >= Tiltrotor_Transition::TRANSITION_TIMER) {
        // we are transitioning to fixed wing - tilt the motors all
        // the way forward
        slew(1);
    } else {
        // until we have completed the transition we limit the tilt to
        // Q_TILT_MAX. Anything above 50% throttle gets
        // Q_TILT_MAX. Below 50% throttle we decrease linearly. This
        // relies heavily on Q_VFWD_GAIN being set appropriately.
       float settilt = constrain_float((SRV_Channels::get_output_scaled(SRV_Channel::k_throttle)-MAX(plane.aparm.throttle_min.get(),0)) / 50.0f, 0, 1);
       slew(settilt * max_angle_deg / 90.0f);
    }
}


/*
  output a slew limited tiltrotor angle. tilt is 0 or 1
 */
void Tiltrotor::binary_slew(bool forward)
{
    // The servo output is binary, not slew rate limited
    SRV_Channels::set_output_scaled(SRV_Channel::k_motor_tilt, forward?1000:0);

    // rate limiting current_tilt has the effect of delaying throttle in tiltrotor_binary_update
    float max_change = tilt_max_change(!forward);
    if (forward) {
        current_tilt = constrain_float(current_tilt+max_change, 0, 1);
    } else {
        current_tilt = constrain_float(current_tilt-max_change, 0, 1);
    }
}

/*
  update motor tilt for binary tilt servos
 */
void Tiltrotor::binary_update(void)
{
    // motors always active
    _motors_active = true;

    if (!quadplane.in_vtol_mode()) {
        // we are in pure fixed wing mode. Move the tiltable motors
        // all the way forward and run them as a forward motor
        binary_slew(true);

        float new_throttle = SRV_Channels::get_output_scaled(SRV_Channel::k_throttle)*0.01f;
        if (current_tilt >= 1) {
            uint8_t mask = is_zero(new_throttle)?0:(uint8_t)tilt_mask.get();
            // the motors are all the way forward, start using them for fwd thrust
            motors->output_motor_mask(new_throttle, mask, plane.rudder_dt);
        }
    } else {
        binary_slew(false);
    }
}


/*
  update motor tilt
 */
void Tiltrotor::update(void)
{
    if (!enabled() || tilt_mask == 0) {
        // no motors to tilt
        return;
    }

    if (type == TILT_TYPE_BINARY) {
        binary_update();
    } else {
        continuous_update();
    }

    if (type == TILT_TYPE_VECTORED_YAW) {
        vectoring();
    }
}

/*
  tilt compensation for angle of tilt. When the rotors are tilted the
  roll effect of differential thrust on the tilted rotors is decreased
  and the yaw effect increased
  We have two factors we apply.

  1) when we are transitioning to fwd flight we scale the tilted rotors by 1/cos(angle). This pushes us towards more flight speed

  2) when we are transitioning to hover we scale the non-tilted rotors by cos(angle). This pushes us towards lower fwd thrust

  We also apply an equalisation to the tilted motors in proportion to
  how much tilt we have. This smoothly reduces the impact of the roll
  gains as we tilt further forward.

  For yaw, we apply differential thrust in proportion to the demanded
  yaw control and sin of the tilt angle

  Finally we ensure no requested thrust is over 1 by scaling back all
  motors so the largest thrust is at most 1.0
 */
void Tiltrotor::tilt_compensate_angle(float *thrust, uint8_t num_motors, float non_tilted_mul, float tilted_mul)
{
    float tilt_total = 0;
    uint8_t tilt_count = 0;
    
    // apply tilt_factors first
    for (uint8_t i=0; i<num_motors; i++) {
        if (!is_motor_tilting(i)) {
            thrust[i] *= non_tilted_mul;
        } else {
            thrust[i] *= tilted_mul;
            tilt_total += thrust[i];
            tilt_count++;
        }
    }

    float largest_tilted = 0;
    const float sin_tilt = sinf(radians(current_tilt*90));
    // yaw_gain relates the amount of differential thrust we get from
    // tilt, so that the scaling of the yaw control is the same at any
    // tilt angle
    const float yaw_gain = sinf(radians(tilt_yaw_angle));
    const float avg_tilt_thrust = tilt_total / tilt_count;

    for (uint8_t i=0; i<num_motors; i++) {
        if (is_motor_tilting(i)) {
            // as we tilt we need to reduce the impact of the roll
            // controller. This simple method keeps the same average,
            // but moves us to no roll control as the angle increases
            thrust[i] = current_tilt * avg_tilt_thrust + thrust[i] * (1-current_tilt);
            // add in differential thrust for yaw control, scaled by tilt angle
            const float diff_thrust = motors->get_roll_factor(i) * motors->get_yaw() * sin_tilt * yaw_gain;
            thrust[i] += diff_thrust;
            largest_tilted = MAX(largest_tilted, thrust[i]);
        }
    }

    // if we are saturating one of the motors then reduce all motors
    // to keep them in proportion to the original thrust. This helps
    // maintain stability when tilted at a large angle
    if (largest_tilted > 1.0f) {
        float scale = 1.0f / largest_tilted;
        for (uint8_t i=0; i<num_motors; i++) {
            thrust[i] *= scale;
        }
    }
}

/*
  choose up or down tilt compensation based on flight mode When going
  to a fixed wing mode we use tilt_compensate_down, when going to a
  VTOL mode we use tilt_compensate_up
 */
void Tiltrotor::tilt_compensate(float *thrust, uint8_t num_motors)
{
    if (current_tilt <= 0) {
        // the motors are not tilted, no compensation needed
        return;
    }
    if (quadplane.in_vtol_mode()) {
        // we are transitioning to VTOL flight
        const float tilt_factor = cosf(radians(current_tilt*90));
        tilt_compensate_angle(thrust, num_motors, tilt_factor, 1);
    } else {
        float inv_tilt_factor;
        if (current_tilt > 0.98f) {
            inv_tilt_factor = 1.0 / cosf(radians(0.98f*90));
        } else {
            inv_tilt_factor = 1.0 / cosf(radians(current_tilt*90));
        }
        tilt_compensate_angle(thrust, num_motors, 1, inv_tilt_factor);
    }
}

/*
  return true if the rotors are fully tilted forward
 */
bool Tiltrotor::fully_fwd(void) const
{
    if (!enabled() || (tilt_mask == 0)) {
        return false;
    }
    return (current_tilt >= 1);
}

/*
  control vectoring for tilt multicopters
 */
void Tiltrotor::vectoring(void)
{
    // total angle the tilt can go through
    const float total_angle = 90 + tilt_yaw_angle + fixed_angle;
    // output value (0 to 1) to get motors pointed straight up
    const float zero_out = tilt_yaw_angle / total_angle;
    const float fixed_tilt_limit = fixed_angle / total_angle;
    const float level_out = 1.0 - fixed_tilt_limit;

    // calculate the basic tilt amount from current_tilt
    float base_output = zero_out + (current_tilt * (level_out - zero_out));
    // for testing when disarmed, apply vectored yaw in proportion to rudder stick
    // Wait TILT_DELAY_MS after disarming to allow props to spin down first.
    constexpr uint32_t TILT_DELAY_MS = 3000;
    uint32_t now = AP_HAL::millis();
    if (!hal.util->get_soft_armed() && (plane.quadplane.options & QuadPlane::OPTION_DISARMED_TILT)) {
        // this test is subject to wrapping at ~49 days, but the consequences are insignificant
        if ((now - hal.util->get_last_armed_change()) > TILT_DELAY_MS) {
            if (quadplane.in_vtol_mode()) {
                float yaw_out = plane.channel_rudder->get_control_in();
                yaw_out /= plane.channel_rudder->get_range();
                float yaw_range = zero_out;

                SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorLeft,  1000 * constrain_float(base_output + yaw_out * yaw_range,0,1));
                SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorRight, 1000 * constrain_float(base_output - yaw_out * yaw_range,0,1));
                SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorRear,  1000 * constrain_float(base_output,0,1));
                SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorRearLeft,  1000 * constrain_float(base_output + yaw_out * yaw_range,0,1));
                SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorRearRight, 1000 * constrain_float(base_output - yaw_out * yaw_range,0,1));
            } else {
                // fixed wing tilt
                const float gain = fixed_gain * fixed_tilt_limit;
                // base the tilt on elevon mixing, which means it
                // takes account of the MIXING_GAIN. The rear tilt is
                // based on elevator
                const float right = gain * SRV_Channels::get_output_scaled(SRV_Channel::k_elevon_right) / 4500.0;
                const float left  = gain * SRV_Channels::get_output_scaled(SRV_Channel::k_elevon_left) / 4500.0;
                const float mid  = gain * SRV_Channels::get_output_scaled(SRV_Channel::k_elevator) / 4500.0;
                // front tilt is effective canards, so need to swap and use negative. Rear motors are treated live elevons.
                SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorLeft,1000 * constrain_float(base_output - right,0,1));
                SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorRight,1000 * constrain_float(base_output - left,0,1));
                SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorRearLeft,1000 * constrain_float(base_output + left,0,1));
                SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorRearRight,1000 * constrain_float(base_output + right,0,1));
                SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorRear,  1000 * constrain_float(base_output + mid,0,1));
            }
        }
        return;
    }

    float tilt_threshold = (max_angle_deg/90.0f);
    bool no_yaw = (current_tilt > tilt_threshold);
    if (no_yaw) {
        // fixed wing  We need to apply inverse scaling with throttle, and remove the surface speed scaling as
        // we don't want tilt impacted by airspeed
        const float scaler = plane.control_mode == &plane.mode_manual?1:(quadplane.FW_vector_throttle_scaling() / plane.get_speed_scaler());
        const float gain = fixed_gain * fixed_tilt_limit * scaler;
        const float right = gain * SRV_Channels::get_output_scaled(SRV_Channel::k_elevon_right) / 4500.0;
        const float left  = gain * SRV_Channels::get_output_scaled(SRV_Channel::k_elevon_left) / 4500.0;
        const float mid  = gain * SRV_Channels::get_output_scaled(SRV_Channel::k_elevator) / 4500.0;
        SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorLeft,1000 * constrain_float(base_output - right,0,1));
        SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorRight,1000 * constrain_float(base_output - left,0,1));
        SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorRearLeft,1000 * constrain_float(base_output + left,0,1));
        SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorRearRight,1000 * constrain_float(base_output + right,0,1));
        SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorRear,  1000 * constrain_float(base_output + mid,0,1));
    } else {
        const float yaw_out = motors->get_yaw();
        const float roll_out = motors->get_roll();
        float yaw_range = zero_out;

        // now apply vectored thrust for yaw and roll.
        const float tilt_rad = radians(current_tilt*90);
        const float sin_tilt = sinf(tilt_rad);
        const float cos_tilt = cosf(tilt_rad);
        // the MotorsMatrix library normalises roll factor to 0.5, so
        // we need to use the same factor here to keep the same roll
        // gains when tilted as we have when not tilted
        const float avg_roll_factor = 0.5;
        const float tilt_offset = constrain_float(yaw_out * cos_tilt + avg_roll_factor * roll_out * sin_tilt, -1, 1);

        SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorLeft,  1000 * constrain_float(base_output + tilt_offset * yaw_range,0,1));
        SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorRight, 1000 * constrain_float(base_output - tilt_offset * yaw_range,0,1));
        SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorRear,  1000 * constrain_float(base_output,0,1));
        SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorRearLeft,  1000 * constrain_float(base_output + tilt_offset * yaw_range,0,1));
        SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorRearRight, 1000 * constrain_float(base_output - tilt_offset * yaw_range,0,1));
    }
}

/*
  control bicopter tiltrotors
 */
void Tiltrotor::bicopter_output(void)
{
    if (type != TILT_TYPE_BICOPTER || quadplane.motor_test.running) {
        // don't override motor test with motors_output
        return;
    }

    if (!quadplane.in_vtol_mode() && fully_fwd()) {
        SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorLeft,  -SERVO_MAX);
        SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorRight, -SERVO_MAX);
        return;
    }

    float throttle = SRV_Channels::get_output_scaled(SRV_Channel::k_throttle);
    if (quadplane.assisted_flight) {
        quadplane.hold_stabilize(throttle * 0.01f);
        quadplane.motors_output(true);
    } else {
        quadplane.motors_output(false);
    }

    // bicopter assumes that trim is up so we scale down so match
    float tilt_left = SRV_Channels::get_output_scaled(SRV_Channel::k_tiltMotorLeft);
    float tilt_right = SRV_Channels::get_output_scaled(SRV_Channel::k_tiltMotorRight);

    if (is_negative(tilt_left)) {
        tilt_left *= tilt_yaw_angle / 90.0f;
    }
    if (is_negative(tilt_right)) {
        tilt_right *= tilt_yaw_angle / 90.0f;
    }

    // reduce authority of bicopter as motors are tilted forwards
    const float scaling = cosf(current_tilt * M_PI_2);
    tilt_left  *= scaling;
    tilt_right *= scaling;

    // add current tilt and constrain
    tilt_left  = constrain_float(-(current_tilt * SERVO_MAX) + tilt_left,  -SERVO_MAX, SERVO_MAX);
    tilt_right = constrain_float(-(current_tilt * SERVO_MAX) + tilt_right, -SERVO_MAX, SERVO_MAX);

    SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorLeft,  tilt_left);
    SRV_Channels::set_output_scaled(SRV_Channel::k_tiltMotorRight, tilt_right);
}

/*
  when doing a forward transition of a tilt-vectored quadplane we use
  euler angle control to maintain good yaw. This updates the yaw
  target based on pilot input and target roll
 */
void Tiltrotor::update_yaw_target(void)
{
    uint32_t now = AP_HAL::millis();
    if (now - transition_yaw_set_ms > 100 ||
        !is_zero(quadplane.get_pilot_input_yaw_rate_cds())) {
        // lock initial yaw when transition is started or when
        // pilot commands a yaw change. This allows us to track
        // straight in transitions for tilt-vectored planes, but
        // allows for turns when level transition is not wanted
        transition_yaw_cd = quadplane.ahrs.yaw_sensor;
    }

    /*
      now calculate the equivalent yaw rate for a coordinated turn for
      the desired bank angle given the airspeed
     */
    float aspeed;
    bool have_airspeed = quadplane.ahrs.airspeed_estimate(aspeed);
    if (have_airspeed && labs(plane.nav_roll_cd)>1000) {
        float dt = (now - transition_yaw_set_ms) * 0.001;
        // calculate the yaw rate to achieve the desired turn rate
        const float airspeed_min = MAX(plane.aparm.airspeed_min,5);
        const float yaw_rate_cds = fixedwing_turn_rate(plane.nav_roll_cd*0.01, MAX(aspeed,airspeed_min))*100;
        transition_yaw_cd += yaw_rate_cds * dt;
    }
    transition_yaw_set_ms = now;
}

bool Tiltrotor_Transition::update_yaw_target(float& yaw_target_cd)
{
    if (!(tiltrotor.is_vectored() &&
        transition_state <= TRANSITION_TIMER)) {
        return false;
    }
    tiltrotor.update_yaw_target();
    yaw_target_cd = tiltrotor.transition_yaw_cd;
    return true;
}

// return true if we should show VTOL view
bool Tiltrotor_Transition::show_vtol_view() const
{
    bool show_vtol = quadplane.in_vtol_mode();

    if (!show_vtol && tiltrotor.is_vectored() && transition_state <= TRANSITION_TIMER) {
        // we use multirotor controls during fwd transition for
        // vectored yaw vehicles
        return true;
    }

    return show_vtol;
}

#endif  // HAL_QUADPLANE_ENABLED
