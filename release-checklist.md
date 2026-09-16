- Replace CHANGE_ME credentials/tokens.
- Reserve LAN IPs in router.
- Change Hub API token.
- Set ArtNet node IP.
- Configure Firebase URL/auth.
- Configure Groq API key and model.
- Verify sensor HTTP reports.
- Verify Nano ACK/DONE and FAULT_SAFE.
- Disable Internet and confirm show continues.
- Power-cycle every device and confirm recovery.
- After any firmware update that changes a default in defaults(): EEPROM
  persists across reflashes and loadConfig() only re-applies defaults() when
  CONFIG_MAGIC changes, so an already-flashed board keeps its old stored
  values forever otherwise. Bump CONFIG_MAGIC (wipes all stored config back
  to current defaults) or manually re-check/update the affected fields via
  /settings.
- Visually check each scene's lighting (Idle/Normal/Fight/Celebration) via
  /settings after updating -- colour/effect/intensity are stored in EEPROM
  and don't need the UI open during a show, but a firmware update that
  changes their defaults (like this one did for Fight) is worth eyeballing
  once before an event, same as any other default change above.
- SD card: organize DFPlayer clips into folders 01-04 (Idle/Normal/Fight/
  Celebration), files named exactly 3 digits + extension per folder --
  see libraries.md. Set each scene's file count via /settings ("Sound
  clips in folder") to match what's actually on the card, or the random
  picker can select a file number that doesn't exist.
- After flashing new Nano firmware or changing Nano actuator config: watch
  the Hub's own Serial output for `[NANO] cfg acked: ...` lines to confirm
  the sync actually reached the Nano, then verify wing speed/mode and lip
  behavior for each scene match what /settings says before relying on it
  live.
- Wing ramp time (Nano Actuators card, "Wing ramp time (ms)") starts at
  600ms -- a starting point, not a tuned value. Watch the actual wing on
  real hardware across a few Normal<->Fight<->Stop transitions and adjust
  until it accelerates/decelerates smoothly without jerking or stalling;
  the right number depends on this wing's specific mass and gearing.
