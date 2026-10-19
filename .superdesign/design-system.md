# SRT dashboard design system

## Product
An offline vehicle instrument display hosted by a classic ESP32, not a fleet management SaaS. Primary task: glance at GPS speed, accelerator input and a local route map. No Internet dependencies, webfonts, large photographs, 3D rendering, or new heavy UI libraries. System sans and inline SVG/CSS only. Identity is the existing SRT text wordmark; there is no SRT image logo. Do not use Vite or React starter assets as branding.

## Current baseline (reproduction only)
Use actual source layout: dark shell, SRT LOCAL TELEMETRY top rail with ESP32 ACCESS POINT and CAN LOG button; speed dial left, GPS summary center, accelerator dial right; offline route map below; detailed telemetry panels at bottom. Preserve this only in the baseline reproduction. Color tokens: background #050807, shell #0d1211, wells #141a1a/#1b2222, borders #232c2c/#1a2121; text #f2f6f6/#9fb0b0/#6c7c7c; cyan #35c4e8/#6fe0f2, green #46d17f, amber #e2a33c, red #e2565b. System UI font, 14px primary radius. Source CSS is authoritative for baseline.

## Requested redesign direction
A complete cockpit redesign, compact and purposeful. Flat graphite surfaces, generous negative space, asymmetric composition, very large tabular numeric speed, a compact segmented accelerator meter, a useful map occupying around half the screen. Avoid the old twin glowing circles, molded shells, extra dashboards or imaginary controls. Small uppercase captions, strong readable numbers. No fabricated battery, range, temperature, camera feed, gear confirmation or motor telemetry. Existing data may be shown only when supported. Missing values use an em dash; disconnected/stale states must be obvious.

Target palette: background #101214; panels #181b1e; raised surface #202428; subtle borders #30363b; primary text #f3f3ed; secondary #a9b0b3; muted #777f85; restrained signal lime #c5ef68; success #75d6a0; warning #efb86b; error #f07878. Retain the system-ui stack; use ui-monospace for diagnostic bytes. Spacing scale 4/8/12/16/24/32/48px; radii 8/12/20px. No gradient backgrounds or neon glow. Responsive 1280x800 landscape, tablet, and 390px phone layouts. At narrow widths stack instruments above the map. Touch targets at least 44px; reduced-motion support and visible keyboard focus.

## Route separation
Homepage /: only the driver display, GPS connection, speed, accelerator, local time, trip distance/heading and offline map. NO CAN LOG button, detailed telemetry, node cards, diagnostic table, status nav, footer link, hidden navigation, or shortcut to /status.
/status: separate manually entered URL for node health, all detailed telemetry panels, and a persistent CAN log table with exact hex ID filter and clear-filter action. It may have a return-to-dashboard link; the reverse link must not exist. Keep raw API endpoints /api/vehicle and /api/can-log intact. This is a view-only UI, not motor control. Node 2 sends no telemetry, so show unavailable rather than offline or invented values.

## Motion
Subtle numeric interpolation and 150ms interactions. No continuous decorative animation. Map fullscreen/close interaction remains functional. Presentation demo values must be clearly illustrative; production uses actual API data.
