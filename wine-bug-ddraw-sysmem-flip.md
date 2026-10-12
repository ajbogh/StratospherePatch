# ddraw: Flip() on a system-memory flipping chain swaps the surfaces; a 1998 application expects the back buffer to be preserved

*Draft for bugs.winehq.org - verify against current master before filing.*

**Component:** ddraw  **Seen with:** Proton 10.0 and Proton Experimental (wined3d, OpenGL renderer)
**Application:** Stratosphere: Conquest of the Skies (1998)

## Summary
The application creates a flipping chain (`DDSCAPS_PRIMARYSURFACE|FLIP|COMPLEX|3DDEVICE|SYSTEMMEMORY`, `dwBackBufferCount = 1`,
640x480x16) and draws a software cursor on the back buffer. Its per-frame cursor erase assumes **`Flip` leaves the back buffer
holding what was just drawn**. With Wine, `IDirectDrawSurface::Flip` exchanges the two surfaces' textures, so the buffer that
comes back as "back buffer" contains the frame from two flips ago. The cursor drawn two frames ago is never erased, so stale copies
of the cursor accumulate on the background.

## Evidence
Per frame the application (all `IDirectDrawSurface2`):
1. `Blt` the old 16x16 area back from a save-under surface into the back buffer
2. `Blt` the 16x16 area under the new position from the back buffer into the save-under surface
3. `Blt` (`DDBLT_KEYSRC`) the cursor sprite onto the back buffer
4. `Flip(NULL, DDFLIP_WAIT)`

I instrumented the `Blt`s (checksum of the destination rect before/after, source checksum) over **130 frames in which the mouse moved**:
in all 130 the restore was issued at exactly the previous cursor rectangle, but in **102** the cursor was *not* present in the back buffer
at that rectangle (only 28 found it). With the mouse stationary it looks fine (both buffers hold the cursor at the same place), which is why
a simple trace misses it. Dumping both buffers (Lock readonly) at a moment when ghosts were visible showed the ghost sprites in **both** the
front and back surfaces, so this is not a presentation problem.
The same application also has a **video-memory** flipping chain (`...3DDEVICE|VIDEOMEMORY`) whose cursor code keeps a two-frame history
(restore uses a capture from two frames earlier) - i.e. it is written for true swapping. That chain works correctly with Wine's swap.

So the application distinguishes the chain types: it expects *copy* behaviour for system-memory chains and *swap* behaviour for video-memory chains.
This is an inference from the application's behaviour (I cannot run it on native Windows here); it matches how DirectDraw's HEL
emulation handled flipping with a system-memory primary, but I have not verified the Windows implementation.

## Workaround that fixes it
Hook `IDirectDrawSurface2::Flip`; if the front surface has `DDSCAPS_SYSTEMMEMORY`, perform `front->Blt(NULL, back, NULL, DDBLT_WAIT, NULL)`
instead of the flip; otherwise call the real `Flip`. The ghosting disappears completely and the rest of the game is unaffected.

## Suggestion
For a flipping chain whose surfaces have `DDSCAPS_SYSTEMMEMORY`, make `Flip` preserve the back buffer (copy semantics) instead of swapping textures.

## Other things seen in the same investigation (not necessarily bugs)
- One frame of the Fortress Builder screen is shown upside-down right after the mode switch (corrected on the next frame).
- When the application leaves exclusive mode (`SetCooperativeLevel(NORMAL)` + `RestoreDisplayMode`) on a multi-monitor desktop, `GetDisplayMode`-based
  checks see 6144x3456 and `ddraw_surface7_Restore` fails with "Display mode dimensions 6144x3456 don't match surface dimensions 640x480",
  leaving the application's surfaces lost (`Cannot SafeLock surface`). A 640x480 display (gamescope or virtual desktop) avoids it.
