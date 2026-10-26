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
//   Resync     at power-up only (startResync): the radio may have been left out of step or
//              with CAT off by the glitch the Arduino's power-up put on its serial line.
//              PC bytes wait unread in the port's buffer, the radio's are dropped, and after
//              CAT_RESYNC_SILENCE_MS of quiet (the radio drops any half-received block after
//              200ms) one CAT on is sent; CAT_RESYNC_SETTLE_MS later it is Normal. Without it
//              flrig's back-to-back polls keep a mis-synchronised radio mis-synchronised and it
//              never answers until something pauses the bus.
//   Drain      the claim is released: queued commands go to the radio in order,
//              spaced like Hamlib spaces them (50ms after a command, 10ms after a
//              query's reply). A new PC command goes straight through if nothing
//              is queued ahead of it, else queues behind, so order is preserved.
//              Back to Normal once the queue is empty.

constexpr uint32_t CAT_RESYNC_SILENCE_MS = 300;
constexpr uint32_t CAT_RESYNC_SETTLE_MS = 200;

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
  enum class Mode : uint8_t { Normal, Hold, Intercept, Drain, Resync };

  explicit CatBridgeCore(CatBridgeIo &io) : io_(io) {}

  // Call once at power-up, after the ports are open: runs the Resync sequence described above.
  void startResync(uint32_t now);

  // Call every loop. True if any byte was seen on either port this pass - the PC's, the
  // radio's, the Arduino's own commands and their replies, replayed commands, and the
  // bridge's made-up answers - for the activity LED.
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
  bool pollResync(uint32_t now);
  void handleEvent(const CatEvent &ev);
  void interceptByte(uint8_t b, uint32_t now);
  void enqueue(const uint8_t frame[CAT_FRAME_LEN]);
  bool busReadyForReplay(uint32_t now) const;
  void sendToRadio(const uint8_t frame[CAT_FRAME_LEN], uint32_t now);
  bool replayOne(uint32_t now); // true if it sent a frame

  CatBridgeIo &io_;
  CatFramer framer_;      // traffic actually forwarded between PC and radio
  CatArbiter arbiter_;
  CatFrameAssembler asm_; // PC commands collected while intercepting
  CatReplayQueue queue_;
  CatSnapshot snapshot_;
  bool snapshotReady_ = false;
  Mode mode_ = Mode::Normal;
  uint32_t lastBusAt_ = 0;
  uint32_t resyncStart_ = 0;
  uint32_t resyncSentAt_ = 0;
  bool resyncSent_ = false;
  uint32_t replayGapMs_ = 0; // quiet needed after the last forwarded frame before the next
};
