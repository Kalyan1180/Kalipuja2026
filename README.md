# ArtNet node
Join the same router as Hub. Use DHCP reservation, e.g. `192.168.0.30`.
Hub sends ArtDMX UDP to port 6454, universe 0 by default. Keep your tested ArtNet/RS485 output implementation; only change its network mode from AP to STA.
Dashboard example: `http://192.168.0.30/`.
