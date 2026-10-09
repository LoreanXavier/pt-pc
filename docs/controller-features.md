# Controller feedback

The PC settings page includes a vibration mode. **Original** is the default and keeps the game's standard motor vibration.
**Enhanced** also enables trigger vibration on compatible controllers and, on supported wired USB DualSense controllers
on Windows, adds vibration derived from Lisa's filtered crying audio. Enhanced effects depend on the controller and its
driver; unsupported effects stay unavailable. Turning the main Vibration setting off disables both original and enhanced
vibration.

Controller-speaker playback is a separate optional setting. On supported wired Sony controllers on Windows, it can play
Lisa's authored crying audio through the controller speaker. Its volume has its own control. The normal game mix,
including headphone output, continues unchanged, and speaker volume does not change vibration strength. The speaker
setting and its volume are independent of the vibration mode.

These controller features have passed software tests for supported-capability handling, event selection and audio
routing. Physical controller endpoint discovery, speaker playback, trigger vibration and DualSense actuator output have
not yet been verified on hardware. Bluetooth and unsupported devices are not claimed to work.
