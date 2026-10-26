#pragma once
#include <stdint.h>

// The MCP2515 driver leaves TXREQ set after a send timeout. Abort the old
// command, stop using the controller, then reset/reconfigure it from loop().
// abortTX() leaves ABAT set in this driver, so begin() MUST follow the abort.
struct CanRecovery {
  static constexpr uint32_t RETRY_MS = 1000, RX_SILENCE_MS = 1500;
  uint32_t faults=0, restarts=0, faultAt=0, lastRxAt=0, pollAt=0, passiveAt=0;
  uint8_t reason=0, errorFlags=0, txErrors=0, rxErrors=0;
  bool pending=false, seenRx=false, passive=false;
  const char *message() const {
    switch(reason) {
      case 1: return "TX timeout/failure";
      case 2: return "bus off";
      case 3: return "persistent error passive";
      case 4: return "RX silent after peer traffic";
      case 5: return "SPI/controller read failure";
      default: return "none";
    }
  }
  void started(uint32_t now) {
    if(pending) ++restarts;
    pending=false; seenRx=false; passive=false; lastRxAt=pollAt=now;
  }
  void received(uint32_t now) { seenRx=true; lastRxAt=now; }
  bool retryDue(uint32_t now, uint32_t lastAttempt) const {
    return now-lastAttempt>=RETRY_MS && (!pending || now-faultAt>=RETRY_MS);
  }
  template<class Controller>
  void fail(Controller &bus, bool &ready, uint8_t why, uint32_t now) {
    if(!ready) return;
    errorFlags=bus.getError(); txErrors=bus.errorCountTX(); rxErrors=bus.errorCountRX();
    reason=why; faultAt=now; pending=true; ++faults; ready=false;
    bus.abortTX(); // Discard queued motion/horn commands before recovery.
  }
  template<class Controller>
  bool send(Controller &bus, bool &ready, uint32_t id, uint8_t len, uint8_t *data, uint32_t now) {
    if(!ready) return false;
    if(bus.sendMsgBuf(id,0,len,data)==0) return true;
    fail(bus,ready,1,now);
    return false;
  }
  template<class Controller>
  void poll(Controller &bus, bool &ready, uint32_t now, bool receivePending=false) {
    if(!ready || now-pollAt<100) return;
    pollAt=now;
    const uint8_t flags=bus.getError();
    // EFLG: TXBO bit5, TXEP/RXEP bits4/3. An overrun alone is historical,
    // not evidence that the controller needs a reset.
    if(flags==0xFF) { fail(bus,ready,5,now); return; }
    if(flags&0x20) { fail(bus,ready,2,now); return; }
    if(flags&0x18) {
      if(!passive) { passive=true; passiveAt=now; }
      else if(now-passiveAt>=300) { fail(bus,ready,3,now); return; }
    } else passive=false;
    if(seenRx && !receivePending && now-lastRxAt>=RX_SILENCE_MS) fail(bus,ready,4,now);
  }
};
