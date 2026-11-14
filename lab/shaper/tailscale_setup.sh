#!/bin/bash
# Make the shaper box a Tailscale subnet router so Paul reaches the whole site network from anywhere.
# Run on the box: sudo bash tailscale_setup.sh 192.168.0.0/24   (Bakhaw: its LAN range)
# Prints a login link the first time; open it, sign in, then approve the route in the Tailscale admin page.
set -e
ROUTES=${1:-192.168.0.0/24}
command -v tailscale >/dev/null || curl -fsSL https://tailscale.com/install.sh | sh
printf 'net.ipv4.ip_forward = 1\nnet.ipv6.conf.all.forwarding = 1\n' > /etc/sysctl.d/99-tailscale.conf
sysctl -p /etc/sysctl.d/99-tailscale.conf
systemctl enable --now tailscaled
# --timeout so this returns with the login link instead of waiting for the sign-in
tailscale up --advertise-routes="$ROUTES" --hostname="$(hostname)" --timeout=15s || true
tailscale status || true
