#pragma once

#include <stdint.h>

#include "cat_arbiter.h"
#include "cat_frame.h"
#include "cat_intercept.h"

// The whole CAT bridge as pure logic, with the serial ports behind CatBridgeIo so
// it runs against fakes in host tests. In normal use it is a byte-transparent
// passthrough between the PC (Port 1) and the radio (Port 2), framing the traffic
// so the Arduino can slot its own commands in (CatArbiter). While a tune cycle
// holds the bus it also keeps the PC supplied with answers:
//
//   Normal     PC bytes forwarded to the radio as they arrive.
//   Hold       the bus is claimed but there is no snapshot yet: PC bytes are left
//              unread in the port's buffer (a few hundred ms at the start of a tune).
//   Intercept  the snapshot is ready: PC bytes are consumed and answered from it
//              (catIntercept) - queries replied, PTT swallowed, the rest queued.
//   Drain      the claim is released: queued commands go to the radio in order,
//              spaced like Hamlib spaces them (50ms after a command, 10ms after a
//              query's reply). A new PC command goes straight through if nothing
//              is queued ahead of it, else queues behind, so order is preserved.
//              Back to Normal once the queue is empty.

class CatBridgeIo {
public:
  virtual int pcRead() = 0;    // next byte from the PC, or -1
  virtual int radioRead() = 0; // next byte from the radio, or -1
  virtual void pcWrite(uint8_t b) = 0;
  virtual void radioWrite(uint8_t b) = 0;

  enum class Frame : uint8_t {
    PcToRadio,      // forwarded PC command
    RadioToPc,      // forwarded radio reply
    StrayFromRadio, // radio byte nobody asked for
    TornFromPc,     // partial PC command dropped
    ReplyTimeout,   // radio never finished a reply
    ArduinoToRadio, // the Arduino's own command
    SynthToPc,      // reply the bridge made up from the snapshot
    SwallowedFromPc,
    QueuedFromPc,
    DroppedFromQueue, // queue full: oldest command lost
    Replayed,         // queued command sent to the radio
  };
  virtual void frame(Frame, const uint8_t *, uint8_t) {}

protected:
  ~CatBridgeIo() {}
};

class CatBridgeCore {
public:
  enum class Mode : uint8_t { Normal, Hold, Intercept, Drain };

  explicit CatBridgeCore(CatBridgeIo &io) : io_(io) {}

  // Call every loop. True if any byte moved in either direction.
  bool poll(uint32_t now);

  // The Arduino's own commands and the long-lived claim (see CatArbiter).
  bool submit(const uint8_t cmd[CAT_FRAME_LEN], uint32_t now) { return arbiter_.submit(cmd, now); }
  bool takeResult(CatArbiter::Result &r, uint8_t *reply, uint8_t &len) {
    return arbiter_.takeResult(r, reply, len);
  }
  bool claim(uint32_t now) { return arbiter_.claim(now); }
  CatArbiter::ClaimState claimState() const { return arbiter_.claimState(); }
  void releaseClaim(uint32_t now) { arbiter_.releaseClaim(now); }

  // The radio's state before the tune. Used from the moment it is set until the
  // claim ends; setting it ends Hold and starts answering the PC.
  void setSnapshot(const CatSnapshot &s);

  Mode mode() const { return mode_; }
  uint8_t queued() const { return queue_.count(); }
  uint16_t queueDropped() const { return queue_.dropped(); }

private:
  void updateMode();
  void handleEvent(const CatEvent &ev);
  void interceptByte(uint8_t b, uint32_t now);
  void enqueue(const uint8_t frame[CAT_FRAME_LEN]);
  bool busReadyForReplay(uint32_t now) const;
  void sendToRadio(const uint8_t frame[CAT_FRAME_LEN], uint32_t now);
  void replayOne(uint32_t now);

  CatBridgeIo &io_;
  CatFramer framer_;      // traffic actually forwarded between PC and radio
  CatArbiter arbiter_;
  CatFrameAssembler asm_; // PC commands collected while intercepting
  CatReplayQueue queue_;
  CatSnapshot snapshot_;
  bool snapshotReady_ = false;
  Mode mode_ = Mode::Normal;
  uint32_t lastBusAt_ = 0;
  uint32_t replayGapMs_ = 0; // quiet needed after the last forwarded frame before the next
};
