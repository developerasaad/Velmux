# Velmux — Design System

Premium, restrained, precise. Must not look like generic AI-generated UI.

## Direction

Use:

- intentional typography with a clear hierarchy;
- coherent spacing and precise alignment;
- restrained color with one accent family;
- excellent interactive states (press/active/disabled are first-class);
- subtle, purposeful motion only;
- clean layouts with real information density;
- real empty / error / loading / recovery states everywhere.

Avoid:

- random gradients; cards everywhere; enormous headings;
- meaningless whitespace; excessive glass; inconsistent radii;
- decorative animation without purpose; dashboard-style clutter.

## Color

Dark-first premium neutrals with a warm accent. No indigo/blue defaults.
Semantic colors: success (green family), warning (amber), danger (red),
progress/info use the accent. Light and dark themes are both supported.

Type scale: compact by default (13–17 pt body on mobile), tabular numerals for
sizes/rates/percentages.

## Frosted glass — selective

Appropriate: floating navigation, compact active-job controls, contextual overlays,
bottom sheets, modal surfaces, transient controls.

Never: entire pages, every card, dense settings forms, long text areas.
On Android where efficient blur is unavailable/expensive, use a visually consistent
translucent/opaque fallback. Never sacrifice responsiveness for blur.

## Navigation

Simple stack + tabs: Home, Queue, History, Settings, with the conversion workspace
as a focused flow. Empty states teach the next action (e.g. "Select a video to
compress").

## Component rules

- Minimum 44 pt touch targets.
- Lists virtualize/recycle for large queues and histories.
- Progress communicates real state; never fake precision (no invented ETAs).
- Every destructive action is confirmable; originals are never silently replaced.
- Accessibility: dynamic type support, labeled controls, sufficient contrast.

## Implementation notes

The shared Compose design system owns tokens (colors, radii, spacing,
type) under `ui/mobile/src/com/velmux/mobile/design`; shared components
under `ui/mobile/src/com/velmux/mobile/components` implement the system
once; screens compose them.
