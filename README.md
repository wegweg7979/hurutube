# hurutube

A small YouTube player: search, watch, and no Shorts. The video plays in mpv with
GPU decode (VA-API).


On Hurunagus it comes from its own COPR (`wegweg7979/hurutube`), so there is
nothing to build there.

## Shorts

Blocked, and not by guessing. YouTube's own search API returns Shorts in a
renderer of their own, so they are never read into the list at all — one real
reply for a single query carried 12 videos and 30 Shorts. A `/shorts/` link is
refused by name instead of played, and a channel listing comes from its `/videos`
tab, which carries none either. Nothing is filtered by duration, so a genuinely
short video is still a video.

## Keys

| Key | What it does |
| :-- | :-- |
| `/` or `Ctrl+L` | focus the search box |
| `Enter` | search, or open a pasted link, or play the highlighted row |
| `↑` `↓` | move through the list |
| `Space` | play the highlighted row, or pause what is playing |
| double click | play the row under the cursor |
| `←` `→` | jump ten seconds |
| `R` | start the current video again |
| `A` | audio only (no video window) |
| `F` | fullscreen |
| `Ctrl+H` | what you have watched; Enter resumes, Delete forgets |
| `?` | every key, in the window |
| `Q` | quit |

