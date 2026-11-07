#!/usr/bin/env bash
# ============================================================================
#  Reach the Aruba AP's web page from the laptop WITHOUT leaving ORBI21:
#  laptop --ethernet--> Pi --linksys222--> AP (192.168.1.10)
#
#     ./deploy/ap_tunnel.sh       then open  https://localhost:4343
#                                 (accept the certificate warning)
#  Ctrl+C closes the tunnel. Needs setup_pi.sh to have run (Pi on linksys222).
# ============================================================================
PI_USER="${PI_USER:-csi}"
PI_ETH="${PI_ETH:-10.42.0.222}"
AP_IP="${AP_IP:-192.168.1.10}"
echo "Tunnel: https://localhost:4343 -> $AP_IP:4343 (and http://localhost:8080 -> :80) via the Pi"
exec ssh -N -o ExitOnForwardFailure=yes -L 4343:"$AP_IP":4343 -L 8080:"$AP_IP":80 "$PI_USER@$PI_ETH"
