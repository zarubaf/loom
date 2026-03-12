# TODO

## Bugs / Oddities

- **Spurious DPI func count in e2e_icap_prog**: `loomx` warns
  `Design has 1 DPI funcs but dispatch only has 0` when running the ICAP
  programming test against a DUT with no DPI calls (`icap_prog.sv` is a plain
  counter). The transformed output appears to register one DPI function that
  shouldn't exist. Investigate whether `emu_top` is incorrectly counting
  something as a DPI cell for this DUT, or whether `loomx` dispatch loading is
  misreporting the count.
