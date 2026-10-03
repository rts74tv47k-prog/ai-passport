<p align="right">
  <a href="companion-storage.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Companion knowledge-base storage

The companion application keeps its persona, categories, and confirmed text
records in the `storage` partition. The partition is FAT on the IDF wear-levelling
driver. Recordings are not kept: only UTF-8 transcripts are written, and only
after the user confirms them. Logs may include ids, byte lengths, and error
codes. They must not include persona text, category names, or record text.

## Partition

| Partition | Type/subtype | Offset | Size | Role |
| --- | --- | ---: | ---: | --- |
| `factory` | app/factory | `0x10000` | `0x400000` (4 MiB) | Application image |
| `storage` | data/fat | `0x410000` | `0x3F0000` (about 3.9 MiB) | Knowledge base |

`storage` is not a preloaded image. The first successful mount formats it when
it does not already contain FAT. A normal application flash stays inside
`factory` and does not erase `storage`. `idf.py erase-flash` erases it.
Mount uses `esp_vfs_fat_spiflash_mount_rw_wl()` at `/store` with
`format_if_mount_failed`. That recovery path formats a filesystem that will not
mount, including a region left behind by an older factory image that used to
occupy this offset.

## Capacity and memory

The record file is capped at 3 MiB so FAT and wear-levelling overhead still fit.
Short transcripts of a few hundred bytes leave room for about 3000 records.
Deleting a record marks it in place. When an append would pass the cap and the
live bytes would fit, the file is rewritten without the marked records. No
single allocation in this module is larger than 8 KiB.

## Files

Names are 8.3 so long-file-name support stays off. Multi-byte text is file
content, not a file name.

| File | Contents |
| --- | --- |
| `PERSONA.TXT` | One persona blob |
| `CATEG.DAT` | 32 fixed category slots |
| `RECORDS.DAT` | Append-only record log |
| `META.DAT` | Next record id cache |

A matching `.NEW` file is the durable temporary for persona, categories, and
the id cache. It is published by replace. On open, a valid temporary replaces
the previous file. `REC.OK` commits a compacted record file: `RECORDS.NEW` is
adopted only when that commit matches its length. An uncommitted temporary is
deleted and the previous record file is kept.

## Record log

Each record is a 24-byte little-endian header plus the transcript:

| Offset | Field |
| ---: | --- |
| 0 | Magic `REC1` |
| 4 | Id, starting at 1 |
| 8 | Category id |
| 12 | Unix time, or 0 when the clock was unset |
| 16 | Text length, 1 to 8192 |
| 18 | Delete mark, first copy |
| 19 | Delete mark, second copy |
| 20 | CRC-32 of the prefix and text, not of the delete marks |

The record is deleted only when both delete marks are `1`. A power loss between
those two bytes leaves the record visible, so the delete can be repeated. A
torn append at the end of the file is truncated on the next open. A record
whose CRC fails but whose length still fits is skipped; later records stay.

## Export text

`companion_files_export_txt()` writes UTF-8 with a leading BOM and LF newlines:

```text
# Categories
- name

# Records
[YYYY-MM-DD HH:MM:SS] name
transcript

```

Unix time 0 is rendered as `[unknown]`. The BOM is what lets Windows Notepad
and WeChat open the file without misreading the text. Category names cannot
contain control characters, so each name stays on one line.
