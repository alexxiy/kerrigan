Wallet
------

- `z_sendmany` has a new 5th argument, `ovk`. With `"sender"`, the shielded
  recipient outputs are encrypted with the source address's outgoing viewing
  key, so the source's viewing key (`z_exportviewingkey`) shows each
  recipient, amount and memo in `z_viewtransaction`, as zcashd and PIVX Core
  do. This is for addresses whose viewing key is published so that their
  payments can be audited. The default, `"none"`, keeps the current
  behaviour: only the recipients can read those outputs. `"sender"` needs a
  shielded `fromaddress`.
