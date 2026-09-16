# v2 network/test plan

Router example: 192.168.0.1
Hub: 192.168.0.10
Entry sensor: 192.168.0.20
Exit sensor: 192.168.0.21
ArtNet: 192.168.0.30
AI monitor: 192.168.0.40

These addresses assume your router's LAN is 192.168.0.x (check with
`ipconfig` on Windows / `ip addr` on Linux / `ifconfig` on macOS — look at
"Default Gateway"). If your router uses a different subnet (e.g.
192.168.1.x, 192.168.68.x, 10.0.0.x), change every IPAddress() constant in
each firmware's config block (Hub, sensor nodes, AI monitor, and the
Hub's sensorIpForNode() lookup) plus the ArtNet node's static IP to match,
keeping the same last-octet scheme (.10/.20/.21/.30/.40).

Sensor -> Hub is local HTTP POST.
AI -> Hub is local HTTP GET/POST.
Hub -> ArtNet is ArtDMX UDP/6454.
Hub -> Nano is UART: CMD (scene triggers, ack/retry'd) and CFG (actuator
config -- wing speed/mode, lip angles/mode, DFPlayer folder file counts;
idempotent, no ack needed, resynced on every Hub boot, every /settings
save, and whenever the Nano recovers from a fault).
Only AI -> Firebase/Groq uses Internet.

Test: Internet OFF must not stop the Hub show.

Hub APIs:
GET /api/status
GET /api/fullStatus
GET /api/config
POST /api/sensor
POST /api/config
POST /api/command
POST /api/scene
