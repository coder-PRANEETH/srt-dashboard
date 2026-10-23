#pragma once
// Included after the Node 5 CAN/HTTP helpers. One HTTP packet updates both axes.
bool joystickCapable() {
  return driveCapable() && fresh(sampleFor(DRIVE_MOTOR_STATUS_ID),250) &&
    steeringNodeReady() && sampleFor(STEERING_STATUS_ID)->bytes[3]==STEERING_PROTOCOL_VERSION;
}
void sendJoystickState() {
  readCan(); serviceDrive(); serviceSteeringRelay();
  const Sample *steer=sampleFor(STEERING_STATUS_ID), *angle=sampleFor(STEERING_ANGLE_ID), *motor=sampleFor(0x201);
  const bool capable=joystickCapable(), ready=capable && driveReady();
  const bool steerLive=steeringNodeReady(), motorLive=fresh(motor,250);
  const bool angleLive=fresh(angle,STEERING_FEEDBACK_TIMEOUT_MS) && steeringAngleFrameFresh(angle->bytes,millis()-angle->at);
  const char *message=drive.reason;
  if(!capable) message="WAITING FOR CONTROL NODES";
  else if(!drive.held && !drivePedalIdle()) message="RELEASE PHYSICAL PEDAL";
  else if(drive.held && !drive.paired) message="TAKING REMOTE CONTROL";
  else if(drive.held && (drive.shiftPhase || (motorLive && motor->bytes[7]==6))) message="DIRECTION CHANGE / SETTLING";
  else if(drive.held) message="REMOTE ACTIVE / PHYSICAL PEDAL BLOCKED";
  String j="{\"ready\":"+String(ready?"true":"false")+",\"token\":"+String(drive.token);
  j+=",\"held\":"+String(drive.held && drive.steeringOwned?"true":"false")+",\"reverse\":"+String(drive.reverse?"true":"false");
  j+=",\"throttlePwm\":"+String(drive.pwm)+",\"steeringDirection\":"+maybeNumber(steerLive,steer->bytes[0]);
  j+=",\"steeringPwm\":"+maybeNumber(steerLive,steer->bytes[1]);
  
  j+=",\"angleDeg\":"+maybeNumber(angleLive,steeringAngleTenths(angle->bytes)/10.0f,1);
  j+=",\"appliedThrottlePwm\":"+maybeNumber(fresh(sampleFor(0x124),250),le16(sampleFor(0x124)->bytes+1));
  j+=",\"powertrainReason\":"+maybeNumber(motorLive,motor->bytes[7]);
  j+=",\"message\":\""+String(message)+"\",\"timeoutMs\":350,\"holdMs\":5000}";
  server.sendHeader("Cache-Control","no-store"); server.send(200,"application/json",j);
}
bool sendJoystickSteering(uint8_t direction,uint8_t pwm,uint16_t seq) {
  const Sample *s=sampleFor(STEERING_STATUS_ID);
  const uint32_t nodeToken=steeringRead32(s->bytes+4);
  if(joystickSteerStopping) {
    // Confirm break-before-make at Node 1 before driving the opposite output.
    if(s->bytes[0] || nodeToken==joystickStoppedNodeToken) return true;
    joystickSteerStopping=false;
  }
  if((steeringRelay.direction && steeringRelay.direction!=direction) || (!direction && s->bytes[0])) {
    joystickStoppedNodeToken=nodeToken;
    stopSteeringRelay(); joystickSteerStopping=true; return true;
  }
  if(!direction) return true;
  if(!steeringRelay.accept(direction,seq,steeringRelay.token,nodeToken,steeringNodeReady(),millis())) return false;
  uint8_t data[8]={direction,pwm,(uint8_t)seq,(uint8_t)(seq>>8),0,0,0,0};
  steeringWrite32(data+4,nodeToken);
  return sendLoggedCan(STEERING_COMMAND_ID,data);
}
void handleJoystickCommand() {
  if(server.header("X-SRT-Control")!="1" || (server.hasHeader("Origin") && server.header("Origin")!="http://"+server.hostHeader())) {
    server.send(403,"application/json","{\"error\":\"Same-origin control required\"}"); return;
  }
  // Validate expiry before accepting a renewal, without sending the old axes.
  readCan(); expireDriveCommand(); serviceSteeringRelay();
  if(server.arg("action")=="stop") { stopDrive(); stopSteeringRelay(); sendJoystickState(); return; }
  uint32_t token=0,seq=0,pwm=0,direction=0,steeringPwm=0,reverse=0;
  if(server.arg("action")!="move" || !steeringDecimal(server.arg("token"),token) ||
      !steeringDecimal(server.arg("seq"),seq) || !seq || seq>65535 ||
      !steeringDecimal(server.arg("throttle"),pwm) || pwm>255 ||
      !steeringDecimal(server.arg("steering"),steeringPwm) || steeringPwm>255 ||
      !steeringDecimal(server.arg("direction"),direction) || direction>2 || (direction ? steeringPwm==0 : steeringPwm!=0) ||
      !steeringDecimal(server.arg("reverse"),reverse) || reverse>1) {
    stopDrive("INVALID JOYSTICK COMMAND"); stopSteeringRelay();
    server.send(400,"application/json","{\"error\":\"Invalid joystick command\"}"); return;
  }
  if(token!=drive.token || !joystickCapable() || !driveReady() ||
      (drive.held && (!drive.steeringOwned || seq<=drive.httpSeq))) {
    stopDrive("RELEASE & RETRY"); stopSteeringRelay();
    server.send(409,"application/json","{\"error\":\"Release and retry\"}"); return;
  }
  if(!drive.held) beginDrive(true,reverse);
  else if(drive.reverse!=bool(reverse)) {
    drive.reverse=reverse; drive.shiftPhase=1; drive.shiftSent=false; drive.lastTx=millis()-50;
  }
  drive.pwm=pwm; drive.percent=(pwm*100+127)/255; drive.httpSeq=seq; drive.lastHttp=millis();
  serviceDrive();
  if(drive.held && drive.paired && !sendJoystickSteering(direction,steeringPwm,seq)) stopDrive("STEERING / CAN FAULT");
  sendJoystickState();
}
