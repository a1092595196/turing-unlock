# Difference between Ampere and Turing

Use the platform-specific row for every payload. There is no single “Ampere”
layout and no global constant offset for all functions.

## DMEM and payload constants

| Item | TU102 | GA100 | GA102 |
| --- | --- | --- | --- |
| WprMeta | `0x400` | `0x600` | `0x400` |
| Signature DMA destination | `0x600` | `0x800` | `0x600` |
| Payload size | `0xfa00` | `0xf800` | `0xfa00` |
| Fill dword | `0x4a7` | `0x4a7` | `0x19c` |
| Global canary | `0x6140` | `0x6340` | `0x6140` |
| f100 restore | DMEM `0x1700` | DMEM `0x1900` | Direct `OR 0x70` |
| Overflow SP | `0xff2c` | `0xff3c` | `0xff30` |
| Return slot | `0xff4c` | `0xff5c` | `0xff50` |

GA100's data segment is generally TU102's `+0x200`; its overflowing frame is
`+0x10`. GA102 shares TU102's DMEM layout but uses a newer code layout.

## Code addresses

| Role | TU102 | GA100 | GA102 |
| --- | --- | --- | --- |
| `dma_copy_block` | `0x04d4` | `0x04d4` | `0x055b` |
| `reg_write_indirect` | `0x10aa` | `0x10aa` | `0x1101` |
| Write mid-entry | `0x10b9` | `0x10b9` | `0x1110` |
| `booterVerifyLsSignatures` | `0x3875` | `0x3747` | `0x3715` |
| `secure_teardown` | `0x7c43` | `0x7e76` | `0x8135` |
| Partial teardown | `0x7cfc` | `0x7f2f` | `0x81ee` |
| Stack-check failure | `0x7ba6` | `0x7dd9` | `0x811b` |
| `main` | `0x7d4f` | `0x7f82` | `0x82ef` |
| `csb_write` | `0x7ff1` | `0x8224` | `0x8592` |
| `csb_read` | `0x8031` | `0x8264` | `0x85d2` |

## Behavioral differences

- TU102 and GA102 check FRTS/WPR2 readiness through `0x11821c` bit17; GA100
  does not have the equivalent `0x91` path.
- All three use finalize state in `0x1180f8`; a normal stock boot can therefore
  make the next fire fail the `0x29` check unless the state is handled.
- GA102 performs additional startup state preservation and f100 handling. Do
  not infer its second-fire behavior from GA100 or TU102.
- Full teardown addresses are not interchangeable. In particular, `0x7cfc` on
  TU102 and `0x7f2f` on GA100 are mid-entry variants, not the full teardown.
