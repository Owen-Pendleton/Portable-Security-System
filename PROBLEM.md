# Problem memo -- Portable Security (Owen Pendleton, Bryan Rosas)

## The user
Macy Pendleton, who travels for work about 3 nights a month and stays in
hotels and short-term rentals. She leaves her laptop, cash, and bags in
the room while she is out for most of the day.

## The problem
When Macy returns to the room, she has no way to know whether anyone
entered, when, or for how long. Her purse has been moved, items have
seemed to be missing, and there was no way to establish what happened.
Housekeeping visits are expected; what's missing is a record that
separates a routine visit from an unexpected one. The cost is worry on
every trip and, when something does go missing, hundreds of dollars.

## Why a device
The room needs watching exactly when Macy isn't in it, and her phone
leaves with her. Cloud cameras (Wyze, Blink, Ring) need an account and
Wi-Fi, but hotel Wi-Fi usually needs a login page these cameras can't
complete, and they upload footage to a company's cloud. An old phone
running a motion app records hours of video nobody reviews, can't tell
an entry from a curtain moving, and dies when its battery or the app
does. A $15 door-stop alarm only makes noise when Macy isn't there to
hear it, and leaves no record. Our device runs fully offline, keeps all
data local, and gives Macy a short list of entry events when she returns.

## The sensors
A PIR motion sensor and a microphone cooperate; a camera adds evidence.
The microphone runs continuously, and our code compares sound against
the room's learned background noise. An entry is declared only when
motion and a characteristic sound (door latch, voices) occur together,
which filters out hallway noise and non-human motion. The camera then
captures still images as evidence for confirmed entries.

## The mechanisms
- B (interrupt-driven input): the PIR is read via a GPIO edge interrupt,
  compared head-to-head against polling for latency and CPU cost.
- D (custom storage layer): the event log and clips must survive a power
  cut mid-write and must not fill the SD card.
- (Added in the design document: E, multi-process supervision, because a
  monitor that silently dies is worse than none.)

## The risk
Storage: audio and image clips can fill the SD card over a 48-hour run
and wear it out. The other serious risk is building sound/motion fusion
that beats a simple threshold without frequent false alarms.
