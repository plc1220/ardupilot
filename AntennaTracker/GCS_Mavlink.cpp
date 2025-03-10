#include "GCS_Mavlink.h"
#include "Tracker.h"

MAV_TYPE GCS_Tracker::frame_type() const
{
    return MAV_TYPE_ANTENNA_TRACKER;
}

MAV_MODE GCS_MAVLINK_Tracker::base_mode() const
{
    uint8_t _base_mode = MAV_MODE_FLAG_CUSTOM_MODE_ENABLED;
    // work out the base_mode. This value is not very useful
    // for APM, but we calculate it as best we can so a generic
    // MAVLink enabled ground station can work out something about
    // what the MAV is up to. The actual bit values are highly
    // ambiguous for most of the APM flight modes. In practice, you
    // only get useful information from the custom_mode, which maps to
    // the APM flight mode and has a well defined meaning in the
    // ArduPlane documentation
    switch (tracker.mode->number()) {
    case Mode::Number::MANUAL:
        _base_mode |= MAV_MODE_FLAG_MANUAL_INPUT_ENABLED;
        break;

    case Mode::Number::STOP:
        break;

    case Mode::Number::SCAN:
    case Mode::Number::SERVOTEST:
    case Mode::Number::AUTO:
    case Mode::Number::GUIDED:
        _base_mode |= MAV_MODE_FLAG_GUIDED_ENABLED |
            MAV_MODE_FLAG_STABILIZE_ENABLED;
        // note that MAV_MODE_FLAG_AUTO_ENABLED does not match what
        // APM does in any mode, as that is defined as "system finds its own goal
        // positions", which APM does not currently do
        break;

    case Mode::Number::INITIALISING:
        break;
    }

    // we are armed if safety switch is not disarmed
    if (hal.util->safety_switch_state() != AP_HAL::Util::SAFETY_DISARMED &&
        tracker.mode != &tracker.mode_initialising &&
        hal.util->get_soft_armed()) {
        _base_mode |= MAV_MODE_FLAG_SAFETY_ARMED;
    }

    return (MAV_MODE)_base_mode;
}

uint32_t GCS_Tracker::custom_mode() const
{
    return (uint32_t)tracker.mode->number();
}

MAV_STATE GCS_MAVLINK_Tracker::vehicle_system_status() const
{
    if (tracker.mode == &tracker.mode_initialising) {
        return MAV_STATE_CALIBRATING;
    }
    return MAV_STATE_ACTIVE;
}

void GCS_MAVLINK_Tracker::send_nav_controller_output() const
{
	float alt_diff = (tracker.g.alt_source == ALT_SOURCE_BARO) ? tracker.nav_status.alt_difference_baro : tracker.nav_status.alt_difference_gps;

    mavlink_msg_nav_controller_output_send(
        chan,
        0,
        tracker.nav_status.pitch,
        tracker.nav_status.bearing,
        tracker.nav_status.bearing,
        MIN(tracker.nav_status.distance, UINT16_MAX),
        alt_diff,
        0,
        0);
}

void GCS_MAVLINK_Tracker::handle_set_attitude_target(const mavlink_message_t &msg)
{
    // decode packet
    mavlink_set_attitude_target_t packet;
    mavlink_msg_set_attitude_target_decode(&msg, &packet);

    // exit if vehicle is not in Guided mode
    if (tracker.mode != &tracker.mode_guided) {
        return;
    }

    // sanity checks:
    if (!is_zero(packet.body_roll_rate)) {
        return;
    }
    if (!(packet.type_mask & (1<<0))) {
        // not told to ignore body roll rate
        return;
    }
    if (!(packet.type_mask & (1<<6))) {
        // not told to ignore throttle
        return;
    }
    if (packet.type_mask & (1<<7)) {
        // told to ignore attitude (we don't allow continuous motion yet)
        return;
    }
    if ((packet.type_mask & (1<<3)) && (packet.type_mask&(1<<4))) {
        // told to ignore both pitch and yaw rates - nothing to do?!
        return;
    }

    const bool use_yaw_rate = !(packet.type_mask & (1<<2));

    tracker.mode_guided.set_angle(
        Quaternion(packet.q[0],packet.q[1],packet.q[2],packet.q[3]),
        use_yaw_rate,
        packet.body_yaw_rate);
}

/*
  send PID tuning message
 */
void GCS_MAVLINK_Tracker::send_pid_tuning()
{
    const Parameters &g = tracker.g;

    // Pitch PID
    if (g.gcs_pid_mask & 1) {
        const AP_PIDInfo *pid_info = &g.pidPitch2Srv.get_pid_info();
        mavlink_msg_pid_tuning_send(chan, PID_TUNING_PITCH,
                                    pid_info->target,
                                    pid_info->actual,
                                    pid_info->FF,
                                    pid_info->P,
                                    pid_info->I,
                                    pid_info->D,
                                    pid_info->slew_rate,
                                    pid_info->Dmod);
        if (!HAVE_PAYLOAD_SPACE(chan, PID_TUNING)) {
            return;
        }
    }

    // Yaw PID
    if (g.gcs_pid_mask & 2) {
        const AP_PIDInfo *pid_info = &g.pidYaw2Srv.get_pid_info();
        mavlink_msg_pid_tuning_send(chan, PID_TUNING_YAW,
                                    pid_info->target,
                                    pid_info->actual,
                                    pid_info->FF,
                                    pid_info->P,
                                    pid_info->I,
                                    pid_info->D,
                                    pid_info->slew_rate,
                                    pid_info->Dmod);
        if (!HAVE_PAYLOAD_SPACE(chan, PID_TUNING)) {
            return;
        }
    }
}

bool GCS_MAVLINK_Tracker::handle_guided_request(AP_Mission::Mission_Command&)
{
    // do nothing
    return false;
}

/*
  default stream rates to 1Hz
 */
const AP_Param::GroupInfo GCS_MAVLINK_Parameters::var_info[] = {
    // @Param: RAW_SENS
    // @DisplayName: Raw sensor stream rate
    // @Description: Raw sensor stream rate to ground station
    // @Units: Hz
    // @Range: 0 50
    // @Increment: 1
    // @RebootRequired: True
    // @User: Advanced
    AP_GROUPINFO("RAW_SENS", 0, GCS_MAVLINK_Parameters, streamRates[0],  1),

    // @Param: EXT_STAT
    // @DisplayName: Extended status stream rate to ground station
    // @Description: Extended status stream rate to ground station
    // @Units: Hz
    // @Range: 0 50
    // @Increment: 1
    // @RebootRequired: True
    // @User: Advanced
    AP_GROUPINFO("EXT_STAT", 1, GCS_MAVLINK_Parameters, streamRates[1],  1),

    // @Param: RC_CHAN
    // @DisplayName: RC Channel stream rate to ground station
    // @Description: RC Channel stream rate to ground station
    // @Units: Hz
    // @Range: 0 50
    // @Increment: 1
    // @RebootRequired: True
    // @User: Advanced
    AP_GROUPINFO("RC_CHAN",  2, GCS_MAVLINK_Parameters, streamRates[2],  1),

    // @Param: RAW_CTRL
    // @DisplayName: Raw Control stream rate to ground station
    // @Description: Raw Control stream rate to ground station
    // @Units: Hz
    // @Range: 0 50
    // @Increment: 1
    // @RebootRequired: True
    // @User: Advanced
    AP_GROUPINFO("RAW_CTRL", 3, GCS_MAVLINK_Parameters, streamRates[3],  1),

    // @Param: POSITION
    // @DisplayName: Position stream rate to ground station
    // @Description: Position stream rate to ground station
    // @Units: Hz
    // @Range: 0 50
    // @Increment: 1
    // @RebootRequired: True
    // @User: Advanced
    AP_GROUPINFO("POSITION", 4, GCS_MAVLINK_Parameters, streamRates[4],  1),

    // @Param: EXTRA1
    // @DisplayName: Extra data type 1 stream rate to ground station
    // @Description: Extra data type 1 stream rate to ground station
    // @Units: Hz
    // @Range: 0 50
    // @Increment: 1
    // @RebootRequired: True
    // @User: Advanced
    AP_GROUPINFO("EXTRA1",   5, GCS_MAVLINK_Parameters, streamRates[5],  1),

    // @Param: EXTRA2
    // @DisplayName: Extra data type 2 stream rate to ground station
    // @Description: Extra data type 2 stream rate to ground station
    // @Units: Hz
    // @Range: 0 50
    // @Increment: 1
    // @RebootRequired: True
    // @User: Advanced
    AP_GROUPINFO("EXTRA2",   6, GCS_MAVLINK_Parameters, streamRates[6],  1),

    // @Param: EXTRA3
    // @DisplayName: Extra data type 3 stream rate to ground station
    // @Description: Extra data type 3 stream rate to ground station
    // @Units: Hz
    // @Range: 0 50
    // @Increment: 1
    // @RebootRequired: True
    // @User: Advanced
    AP_GROUPINFO("EXTRA3",   7, GCS_MAVLINK_Parameters, streamRates[7],  1),

    // @Param: PARAMS
    // @DisplayName: Parameter stream rate to ground station
    // @Description: Parameter stream rate to ground station
    // @Units: Hz
    // @Range: 0 50
    // @Increment: 1
    // @RebootRequired: True
    // @User: Advanced
    AP_GROUPINFO("PARAMS",   8, GCS_MAVLINK_Parameters, streamRates[8],  10),
    AP_GROUPEND
};

static const ap_message STREAM_RAW_SENSORS_msgs[] = {
    MSG_RAW_IMU,
    MSG_SCALED_IMU2,
    MSG_SCALED_IMU3,
    MSG_SCALED_PRESSURE,
    MSG_SCALED_PRESSURE2,
    MSG_SCALED_PRESSURE3,
};
static const ap_message STREAM_EXTENDED_STATUS_msgs[] = {
    MSG_SYS_STATUS,
    MSG_POWER_STATUS,
    MSG_MCU_STATUS,
    MSG_MEMINFO,
    MSG_NAV_CONTROLLER_OUTPUT,
    MSG_GPS_RAW,
    MSG_GPS_RTK,
    MSG_GPS2_RAW,
    MSG_GPS2_RTK,
};
static const ap_message STREAM_POSITION_msgs[] = {
    MSG_LOCATION,
    MSG_LOCAL_POSITION
};
static const ap_message STREAM_RAW_CONTROLLER_msgs[] = {
    MSG_SERVO_OUTPUT_RAW,
};
static const ap_message STREAM_RC_CHANNELS_msgs[] = {
    MSG_RC_CHANNELS,
    MSG_RC_CHANNELS_RAW, // only sent on a mavlink1 connection
};
static const ap_message STREAM_EXTRA1_msgs[] = {
    MSG_ATTITUDE,
    MSG_PID_TUNING,
};
static const ap_message STREAM_EXTRA3_msgs[] = {
    MSG_AHRS,
    MSG_SIMSTATE,
    MSG_SYSTEM_TIME,
    MSG_AHRS2,
    MSG_MAG_CAL_REPORT,
    MSG_MAG_CAL_PROGRESS,
    MSG_EKF_STATUS_REPORT,
};
static const ap_message STREAM_PARAMS_msgs[] = {
    MSG_NEXT_PARAM
};

const struct GCS_MAVLINK::stream_entries GCS_MAVLINK::all_stream_entries[] = {
    MAV_STREAM_ENTRY(STREAM_RAW_SENSORS),
    MAV_STREAM_ENTRY(STREAM_EXTENDED_STATUS),
    MAV_STREAM_ENTRY(STREAM_POSITION),
    MAV_STREAM_ENTRY(STREAM_RAW_CONTROLLER),
    MAV_STREAM_ENTRY(STREAM_RC_CHANNELS),
    MAV_STREAM_ENTRY(STREAM_EXTRA1),
    MAV_STREAM_ENTRY(STREAM_EXTRA3),
    MAV_STREAM_ENTRY(STREAM_PARAMS),
    MAV_STREAM_TERMINATOR // must have this at end of stream_entries
};

/*
  We eavesdrop on MAVLINK_MSG_ID_GLOBAL_POSITION_INT and
  MAVLINK_MSG_ID_SCALED_PRESSUREs
*/
void GCS_MAVLINK_Tracker::packetReceived(const mavlink_status_t &status,
                                         const mavlink_message_t &msg)
{
    // return immediately if sysid doesn't match our target sysid
    if ((tracker.g.sysid_target != 0) && (tracker.g.sysid_target != msg.sysid)) {
        GCS_MAVLINK::packetReceived(status, msg);
        return;
    }

    switch (msg.msgid) {
    case MAVLINK_MSG_ID_HEARTBEAT:
    {
        mavlink_check_target(msg);
        break;
    }

    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT:
    {
        // decode
        mavlink_global_position_int_t packet;
        mavlink_msg_global_position_int_decode(&msg, &packet);
        tracker.tracking_update_position(packet);
        break;
    }
    
    case MAVLINK_MSG_ID_SCALED_PRESSURE:
    {
        // decode
        mavlink_scaled_pressure_t packet;
        mavlink_msg_scaled_pressure_decode(&msg, &packet);
        tracker.tracking_update_pressure(packet);
        break;
    }
    }
    GCS_MAVLINK::packetReceived(status, msg);
}

// locks onto a particular target sysid and sets it's position data stream to at least 1hz
void GCS_MAVLINK_Tracker::mavlink_check_target(const mavlink_message_t &msg)
{
    // exit immediately if the target has already been set
    if (tracker.target_set) {
        return;
    }

    // decode
    mavlink_heartbeat_t packet;
    mavlink_msg_heartbeat_decode(&msg, &packet);

    // exit immediately if this is not a vehicle we would track
    if ((packet.type == MAV_TYPE_ANTENNA_TRACKER) ||
        (packet.type == MAV_TYPE_GCS) ||
        (packet.type == MAV_TYPE_ONBOARD_CONTROLLER) ||
        (packet.type == MAV_TYPE_GIMBAL)) {
        return;
    }

    // set our sysid to the target, this ensures we lock onto a single vehicle
    if (tracker.g.sysid_target == 0) {
        tracker.g.sysid_target.set(msg.sysid);
    }

    // send data stream request to target on all channels
    //  Note: this doesn't check success for all sends meaning it's not guaranteed the vehicle's positions will be sent at 1hz
    tracker.gcs().request_datastream_position(msg.sysid, msg.compid);
    tracker.gcs().request_datastream_airpressure(msg.sysid, msg.compid);

    // flag target has been set
    tracker.target_set = true;
}

uint8_t GCS_MAVLINK_Tracker::sysid_my_gcs() const
{
    return tracker.g.sysid_my_gcs;
}

MAV_RESULT GCS_MAVLINK_Tracker::_handle_command_preflight_calibration_baro(const mavlink_message_t &msg)
{
    MAV_RESULT ret = GCS_MAVLINK::_handle_command_preflight_calibration_baro(msg);
    if (ret == MAV_RESULT_ACCEPTED) {
        // zero the altitude difference on next baro update
        tracker.nav_status.need_altitude_calibration = true;
    }
    return ret;
}

MAV_RESULT GCS_MAVLINK_Tracker::handle_command_component_arm_disarm(const mavlink_command_int_t &packet)
{
    if (is_equal(packet.param1,1.0f)) {
        tracker.arm_servos();
        return MAV_RESULT_ACCEPTED;
    }
    if (is_zero(packet.param1))  {
        tracker.disarm_servos();
        return MAV_RESULT_ACCEPTED;
    }
    return MAV_RESULT_UNSUPPORTED;
}

MAV_RESULT GCS_MAVLINK_Tracker::handle_command_long_packet(const mavlink_command_long_t &packet, const mavlink_message_t &msg)
{
    switch(packet.command) {

    case MAV_CMD_DO_SET_SERVO:
        // ensure we are in servo test mode
        tracker.set_mode(tracker.mode_servotest, ModeReason::SERVOTEST);

        if (!tracker.mode_servotest.set_servo(packet.param1, packet.param2)) {
            return MAV_RESULT_FAILED;
        }
        return MAV_RESULT_ACCEPTED;

        // mavproxy/mavutil sends this when auto command is entered 
    case MAV_CMD_MISSION_START:
        tracker.set_mode(tracker.mode_auto, ModeReason::GCS_COMMAND);
        return MAV_RESULT_ACCEPTED;

    default:
        return GCS_MAVLINK::handle_command_long_packet(packet, msg);
    }
}

bool GCS_MAVLINK_Tracker::set_home_to_current_location(bool _lock) {
    return tracker.set_home(AP::gps().location());
}
bool GCS_MAVLINK_Tracker::set_home(const Location& loc, bool _lock) {
    return tracker.set_home(loc);
}

void GCS_MAVLINK_Tracker::handleMessage(const mavlink_message_t &msg)
{
    switch (msg.msgid) {

    case MAVLINK_MSG_ID_SET_ATTITUDE_TARGET:
        handle_set_attitude_target(msg);
        break;

    // When mavproxy 'wp sethome' 
    case MAVLINK_MSG_ID_MISSION_WRITE_PARTIAL_LIST:
    {
        // decode
        mavlink_mission_write_partial_list_t packet;
        mavlink_msg_mission_write_partial_list_decode(&msg, &packet);
        if (packet.start_index == 0)
        {
            // New home at wp index 0. Ask for it
            waypoint_receiving = true;
            send_message(MSG_NEXT_MISSION_REQUEST_WAYPOINTS);
        }
        break;
    }

    // XXX receive a WP from GCS and store in EEPROM if it is HOME
    case MAVLINK_MSG_ID_MISSION_ITEM:
    {
        // decode
        mavlink_mission_item_t packet;
        MAV_MISSION_RESULT result = MAV_MISSION_ACCEPTED;

        mavlink_msg_mission_item_decode(&msg, &packet);

        Location tell_command;

        switch (packet.frame)
        {
        case MAV_FRAME_MISSION:
        case MAV_FRAME_GLOBAL:
        {
            tell_command = Location{
                int32_t(1.0e7f*packet.x), // in as DD converted to * t7
                int32_t(1.0e7f*packet.y), // in as DD converted to * t7
                int32_t(packet.z*1.0e2f), // in as m converted to cm
                Location::AltFrame::ABSOLUTE
            };
            break;
        }

#ifdef MAV_FRAME_LOCAL_NED
        case MAV_FRAME_LOCAL_NED:                         // local (relative to home position)
        {
            tell_command = Location{
                int32_t(1.0e7f*ToDeg(packet.x/(RADIUS_OF_EARTH*cosf(ToRad(home.lat/1.0e7f)))) + home.lat),
                int32_t(1.0e7f*ToDeg(packet.y/RADIUS_OF_EARTH) + home.lng),
                int32_t(-packet.z*1.0e2f),
                Location::AltFrame::ABOVE_HOME
            };
            break;
        }
#endif

#ifdef MAV_FRAME_LOCAL
        case MAV_FRAME_LOCAL:                         // local (relative to home position)
        {
            tell_command = {
                int32_t(1.0e7f*ToDeg(packet.x/(RADIUS_OF_EARTH*cosf(ToRad(home.lat/1.0e7f)))) + home.lat),
                int32_t(1.0e7f*ToDeg(packet.y/RADIUS_OF_EARTH) + home.lng),
                int32_t(packet.z*1.0e2f),
                Location::AltFrame::ABOVE_HOME
            };
            break;
        }
#endif

        case MAV_FRAME_GLOBAL_RELATIVE_ALT:                         // absolute lat/lng, relative altitude
        {
            tell_command = {
                int32_t(1.0e7f * packet.x), // in as DD converted to * t7
                int32_t(1.0e7f * packet.y), // in as DD converted to * t7
                int32_t(packet.z * 1.0e2f),
                Location::AltFrame::ABOVE_HOME
            };
            break;
        }

        default:
            result = MAV_MISSION_UNSUPPORTED_FRAME;
            break;
        }

        if (result != MAV_MISSION_ACCEPTED) goto mission_failed;

        // Check if receiving waypoints (mission upload expected)
        if (!waypoint_receiving) {
            result = MAV_MISSION_ERROR;
            goto mission_failed;
        }

        // check if this is the HOME wp
        if (packet.seq == 0) {
            if (!tracker.set_home(tell_command)) {
                result = MAV_MISSION_ERROR;
                goto mission_failed;
            }
            send_text(MAV_SEVERITY_INFO,"New HOME received");
            waypoint_receiving = false;
        }

mission_failed:
        // we are rejecting the mission/waypoint
        mavlink_msg_mission_ack_send(
            chan,
            msg.sysid,
            msg.compid,
            result,
            MAV_MISSION_TYPE_MISSION);
        break;
    }

    case MAVLINK_MSG_ID_MANUAL_CONTROL:
    {
        mavlink_manual_control_t packet;
        mavlink_msg_manual_control_decode(&msg, &packet);
        tracker.tracking_manual_control(packet);
        break;
    }

    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: 
    {
        // decode
        mavlink_global_position_int_t packet;
        mavlink_msg_global_position_int_decode(&msg, &packet);
        tracker.tracking_update_position(packet);
        break;
    }

    case MAVLINK_MSG_ID_SCALED_PRESSURE: 
    {
        // decode
        mavlink_scaled_pressure_t packet;
        mavlink_msg_scaled_pressure_decode(&msg, &packet);
        tracker.tracking_update_pressure(packet);
        break;
    }

    default:
        handle_common_message(msg);
        break;
    } // end switch
} // end handle mavlink


// send position tracker is using
void GCS_MAVLINK_Tracker::send_global_position_int()
{
    if (!tracker.stationary) {
        GCS_MAVLINK::send_global_position_int();
        return;
    }

    mavlink_msg_global_position_int_send(
        chan,
        AP_HAL::millis(),
        tracker.current_loc.lat,  // in 1E7 degrees
        tracker.current_loc.lng,  // in 1E7 degrees
        tracker.current_loc.alt,  // millimeters above ground/sea level
        0,                        // millimeters above home
        0,                        // X speed cm/s (+ve North)
        0,                        // Y speed cm/s (+ve East)
        0,                        // Z speed cm/s (+ve Down)
        tracker.ahrs.yaw_sensor); // compass heading in 1/100 degree
}
