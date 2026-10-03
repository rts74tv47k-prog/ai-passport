<p align="right"><a href="PRD.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Private AI Companion · Product Requirements (PRD)

The Simplified Chinese document is the wording authority for on-device strings. This English file states the same behavior for the repository language pair.

| Item | Content |
| --- | --- |
| Document version | v1.1 |
| Status | Confirmed. Sole requirements source for the MVP. |
| Target hardware | FoloToy AI Passport (ESP32-C3) |
| Baseline | Fork of the official repository, branch `feature/companion-mvp` |

## 1. Product definition

One sentence: a private AI companion that lives on AI Passport. It talks with the owner from a persona, and it turns spoken notes into an exportable personal knowledge base.

Two core paths:

- Talk: persona-driven model conversation, with both spoken and written replies
- Keep: hold a button to speak, transcribe, let the model classify (including a spoken new category), store locally, then export a txt file on the LAN

The product is minimal, restrained, and private. It is a small daily tool, not a platform and not a social product.

## 2. Users and situations

- User: one person, the owner. No accounts.
- Environment: Wi-Fi, including a phone hotspot. First join uses the official Bluetooth provisioning flow.
- Typical use: a short chat in a spare moment; a long-press note after sport; a spoken reading note before sleep; a weekend txt export on a computer.

## 3. Hardware and constraints

| Capability | Specification | Product effect |
| --- | --- | --- |
| Screen | 240x320 portrait | UI is designed for this panel. Chinese needs a dedicated font. |
| Buttons | UP / DOWN / OK (short, long, and double press are detectable) | All interaction uses these three buttons. |
| Audio | Microphone and speaker | Voice input and spoken replies |
| Network | Wi-Fi 2.4 GHz and BLE | ASR, LLM, and TTS use a cloud API |
| Storage | 8 MB Flash, no PSRAM | Store text only. Discard raw audio. |
| Battery | Built-in fuel gauge | Battery badge stays available |

Constraints:

1. Network is required for speech recognition, chat, and speech synthesis. Offline, browsing saved records still works. Chat and record entry show a clear offline notice.
2. No wake word. The ESP32-C3 cannot keep the microphone open. A button press is the wake action.
3. The UI must be original. Do not reuse the baseline test menu or pages. BSP drivers and non-UI logic may be reused.
4. Memory is tight. There is no PSRAM. The figure uses a cheap frame animation. One heap block stays at or below 8 KB. A later streaming capture buffer is counted separately.

## 4. Functional requirements

### 4.1 Home (default screen)

- A fixed companion figure, independent of the persona text. Abstract and minimal.
- Idle motion: breathing, plus an occasional blink.
- A bubble at the bottom shows the latest reply.
- Status badges at the top right: battery (always shown when the reading succeeds; hidden cleanly when it fails) and Wi-Fi state.

### 4.2 Chat

- The persona is one text prompt, edited on the web page. The device has no keyboard.
- Flow: short-press OK to start, the figure switches to a listening pose, short-press OK again to finish, then the bubble and the speaker reply together, and the figure plays a response pose.
- Context keeps the latest 5 turns by default. The default may be changed later.
- Chat history is not stored. Only the record path writes the knowledge base.

### 4.3 Voice records

Four states:

1. Listen (its own page): enter with a long press on OK. The figure leans in, a level bar moves, and a timer runs. The long-press threshold is about 0.5 s, so a short delay before the page appears is expected.
2. Processing: after release, the level bar stops and the page shows a recognizing status. Transcription and classification are one model call.
3. Confirm (its own page): show the transcript and the suggested category. UP / DOWN change the category. A newly proposed category is first. The rest follow recent use. Short-press OK saves and returns home. Long-press OK discards and returns home. There is no timeout auto-save. The page waits. After idle time the screen dims.
4. Done: return home with a light success cue (a nod or a short tone).

One recording stops at 60 seconds by default, then shows that it was cut off.

### 4.4 Categories

- Three categories exist at first: Daily, Tennis, and Reading. The on-device names are the Chinese strings in the Chinese PRD.
- The model receives the transcript and the current category list in one call and chooses an existing category, a new category, or the Daily default.
- A spoken example creates a new category and files the note there.
- The proposed new name always appears on the confirm page, so a bad transcript can be seen and discarded.
- The list is ordered by recent use. The web page can view, create, and rename categories.

### 4.5 Browse

- From home, UP or DOWN opens browse.
- UP / DOWN move through history. Records are shown with their category, time, and text.
- Short-press OK expands or collapses a long transcript. Long-press OK returns home.
- View only. Editing happens after export, on a computer.

### 4.6 LAN web page

Phone or computer on the same Wi-Fi opens the device address:

| Function | Behavior |
| --- | --- |
| Wi-Fi | Set STA credentials. A device-side provisioning entry is also allowed in phase 3. |
| Persona | A large text field. Saving applies it. |
| API key | Zhipu API key, stored in device NVS, never in the repository. |
| Categories | View, create, and rename. |
| Records | Paged view of every record. |
| Export txt | By category or all records. UTF-8 with BOM. |
| Storage | Show used space. Near the cap, ask the user to export and clean up. |

### 4.7 Global rules

- Chat and recording are exclusive. One in progress blocks the other.
- Failures such as offline or an API error stay quiet. Show status. Browse still works.

## 5. Button map

### Home

| Action | Behavior |
| --- | --- |
| OK short | Start or stop chat speech |
| OK long | Open the listen page |
| UP / DOWN | Open browse |

### Listen

| Action | Behavior |
| --- | --- |
| Release OK | Stop capture, then process, then open confirm |

### Confirm

| Action | Behavior |
| --- | --- |
| UP / DOWN | Change category |
| OK short | Save and return home |
| OK long | Discard and return home |

### Browse

| Action | Behavior |
| --- | --- |
| UP / DOWN | Previous / next record |
| OK short | Expand or collapse the full text |
| OK long | Return home |

## 6. Data

### 6.1 Record fields

| Field | Type | Meaning |
| --- | --- | --- |
| Timestamp | datetime | Minute precision, clock set by SNTP |
| Category | text | Category name |
| Body | text | Transcript |
| Sequence | int | Increasing, so export stays ordered |

### 6.2 Export layout

File name `passport-notes-YYYY-MM-DD.txt`, UTF-8 with BOM. Each category is a heading, then lines of `[MM-DD HH:MM]` and the transcript. The sample wording is in the Chinese PRD.

### 6.3 Storage (phase 1 implementation; do not rewrite)

- The `storage` partition sits at the end of Flash (`0x410000`, about 3.9 MB), using the ESP-IDF FAT filesystem and wear levelling.
- Records append. Delete uses two flag bytes, so a torn delete can be retried. A torn tail is truncated on the next open.
- Raw audio is not stored. One heap block stays at or below 8 KB.
- Capacity is about 3000 records. The web page shows usage and asks for export and cleanup near the cap.

### 6.4 Privacy

- Records stay on the device. Content is sent to the chosen AI API only while processing.
- No cloud store and no third-party upload. The web page is LAN-only.
- The API key lives in device NVS, not in the repository. Logs print ids, lengths, and error codes only. They do not print conversation text or record bodies.

## 7. Experience

1. Keep it small: the device UI plus one web page.
2. Restrained visuals: a minimal abstract figure, no extra decoration.
3. Immediate feedback: every button action has a visible response.
4. Quiet failure: status and hints, not alarms.

## 8. Out of scope for v1

| Item | Reason |
| --- | --- |
| Wake word | The C3 cannot keep the microphone listening |
| Offline chat or new records | ASR and the LLM need the cloud |
| Cloud sync or backup | Conflicts with the privacy goal. LAN export is enough. |
| Raw audio storage | Flash holds text |
| Record editing | Discard only. Organize after export. |
| Record search | Consider in v2 |

## 9. Acceptance

- [ ] One minute of speech, release, confirm page visible, within 10 seconds end to end
- [ ] File into an existing category, create a category by voice, change category, and discard all work
- [ ] Every record needs a manual OK. Confirm never auto-saves on a timeout.
- [ ] Records and categories survive power loss
- [ ] A browser can edit the persona, manage categories, and export txt that opens in WeChat and Notepad
- [ ] Offline chat and record show a clear notice. Browse still works.
- [ ] The battery badge is visible whenever a reading exists, and it warns below 20%
- [ ] The UI is original and is not the baseline test menu or pages

## 10. Technical choices

| Topic | Choice |
| --- | --- |
| AI API | Zhipu bigmodel.cn: glm-asr for speech recognition, glm-4-flash for chat and classification, cogtts for speech synthesis. The response may be wav or mp3. The player supports wav only. A non-wav response skips playback and shows text. |
| Provisioning | Decided in phase 3. Prefer the official flow, with SoftAP web provisioning as fallback. |
| UI | LVGL from the official baseline. Chinese text uses an LVGL built-in CJK font. |
| Firmware | ESP-IDF 5.5.3. Reuse BSP drivers. Application code stays in `main/`. |
| Versions | GitHub fork, branch `feature/companion-mvp`, tags per iteration |

## 11. Open questions

- [ ] Measure whether cogtts returns wav or mp3
- [ ] Whether the web page needs a simple access code (not in v1)
- [ ] Final figure artwork (minimal abstract direction, budget of 2-3 animation frames)

## 12. Later ideas (v2+, not committed)

Record search and an "on this day" review; more than one persona; the companion speaking first; optionally saving chat into the knowledge base; LAN OTA firmware updates.

## Revision history

| Version | Date | Change |
| --- | --- | --- |
| v1.0 | 2026-09-29 | First edition |
| v1.1 | 2026-10-03 | Restored sections 1 through 6.2. Section 6.3 matches the committed phase 1 store. Section 10 selects Zhipu. |
