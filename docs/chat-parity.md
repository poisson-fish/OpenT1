# Chat entry and score-menu parity

Audited 2026-11-04 against the native sources in
`../TribesRE/legacy/tribes-rebirth`. The relevant paths are
`program/code/dlgPlay.cpp`, `program/code/FearGuiChat.cpp`,
`program/code/chatmenu.cpp`, and `darkstar/SimGui/code/simGuiCanvas.cpp`.

| Native behavior | OpenT1 correction |
| --- | --- |
| `playDelegate::onSimActionEvent(IDACTION_CHAT)` finds chat on `curGui`, the content GUI, even while `score.gui` is pushed above it. | Chat activation and chat-display actions resolve the underlying gameplay GUI while the score dialog is open. Other dialogs retain their existing modal routing. |
| `Canvas::makeFirstResponder` allows that content chat to receive keyboard events under the score dialog. | Covered keyboard focus is explicitly enabled for chat. Tab-list rebuilds retain it; event rollback includes the covered graph. Mouse capture and ordinary tab navigation still target the top dialog. |
| The GUI consumes text before the lower-priority `ChatMenu` and action-map consumers. | The host's existing keyboard-active gate now sees the covered chat, so entering a number cannot select a server-menu row. Closing chat restores menu input without dismissing the score dialog. |
| The chat action writes `pref::msgChannel`; the entry control uses channels 0–31. | Activation updates the preference and clamps the displayed/submitted channel. |
| `formatMessage` wraps at spaces, reserves the prompt only on the first row, and falls back to splitting long words. Typing grows the panel; submitting resets it to two rows. | Layout and drawing share the same wrapping calculation. Continuation rows use the full text inset. Long messages and recalled history remain visible. |
| Native entry width is 370 pixels normally and at least 200 in low resolution, with a 10-pixel vertical border. | Restored the compact width, panel height, font-dependent line spacing, and font scale. |
| Native high-resolution text starts two pixels above the panel origin; its caret starts three pixels below that text origin and extends one font height. The text clip is inset four pixels vertically. Low resolution uses zero text/caret offsets. | Restored the text baseline, clipping, and full-height caret, including the empty-entry case. |
| Escape saves the unfinished draft. The first Backspace/Delete after activation clears it. Both history directions cycle through the saved lines and the empty slot. | Restored draft retention, first-delete handling, and history cycling. Reopening resets selection and cursor blink. |

The continuation text stays inside the text inset rather than reproducing the
native renderer's clipped placement at the panel's left edge. Wrapping also
preserves UTF-8 character boundaries and makes progress in very narrow controls.

`game/rpg_paths` covers activation through keyboard action maps with an active
server menu and a serialized score dialog, public/team submission, numeric text,
Escape, restored menu selection, draft/history behavior, word/long-word wrapping,
panel growth/reset, covered-event rollback, and normal/low-resolution/scaled caret
pixels.

The optional `chat/installed_rpg_bulk_chat` uses `OPENT1_TEST_ASSETS` and a
temporary profile/write directory. It opens the installed RPG skill-point menu,
activates chat through the installed `t` binding, submits `5`, verifies the
server's `bulkNum`, and selects a skill through the menu. The expected result is
five skill credits spent while the menu remains open. Its server uses an
automatically assigned port so it can run beside an existing game.

Run it with `opent1_tests --suite chat` after setting `OPENT1_TEST_ASSETS` to
the RPG installation root.

Validation on 2026-11-04: Debug and Release both passed the GUI suite,
`game/rpg_paths`, and the installed five-credit bulk-use acceptance case. The
release executable also passed the asset-free headless startup check. Both full
game-suite runs had one failure in the concurrently modified
`sound_loop_emitters` audio test.
