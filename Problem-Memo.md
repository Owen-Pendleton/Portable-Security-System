# Problem memo -- Portable Security (Owen Pendleton, Bryan Rosas>)

## The user
Macy Pendleton, who travels for work about 3 nights a month
and stays in hotels and short-term rentals. They leave laptop, money,
purses, etc in the room while out for most of the day.

## The problem
When Macy returns to the room, they have no way to know whether anyone
entered, when, or for how long. Personal belongings like her purse
had been moved, items seemed to be missing, and there was no way to 
establish what happened. Housekeeping visits are expected, but what's missing 
is a record that separates a routine visit from an unexpected one. The cost
is worry on every trip that this may happen again and hundreds of dollars may 
be lost.

## Why a device
The room needs watching exactly when the traveler isn't in it, and their
phone leaves with them. Commercial cameras (Wyze, Blink, Ring) need an
app account and Wi-Fi, but hotel Wi-Fi typically requires a login page
these cameras can't complete, and they upload footage to a company's
cloud. Our device runs fully offline, keeps all data local, and produces
a short list of entry events instead of hours of footage nobody reviews.

## The sensors
A PIR motion sensor, a microphone, and a camera. The microphone runs
continuously into a short rolling buffer, and our code compares sound
against the room's learned background noise. An entry is declared only
when motion and a characteristic sound (door latch, voices) occur
together, which filters out hallway noise and non-human motion. The
camera then captures a short clip as evidence for confirmed events.

## The mechanisms
- B (interrupt-driven input): the PIR is read via a GPIO edge interrupt,
  compared head-to-head against polling for latency and CPU cost, since
  a battery-conscious portable device shouldn't spin on a pin.
- D (custom storage layer): event clips and the event log must survive a
  power cut mid-write and must not fill the SD card, so we need explicit
  retention, write batching, and a crash-recovery story.

## The risk
Storage: audio and video clips can fill the SD card quickly over a
48-hour run and wear it out with frequent writes. The other serious risk
is building sound/motion fusion that beats a simple threshold without
frequent false alarms.