Wallet
------

- `z_sendmany` has a new 6th argument, `change`. With `"source"`, shielded
  change goes back to `fromaddress` itself, encrypted with its outgoing
  viewing key, as zcashd does for Sapling addresses and PIVX Core does. The
  source's viewing key alone then shows the change and the address's
  balance. `z_viewtransaction` lists that output as one to `fromaddress`
  with `change: false`. The default, `"internal"`, keeps the current
  behaviour: change goes to a new address of the source key's internal
  (ZIP 316) key, which cannot be linked to `fromaddress`. `"source"` needs a
  shielded `fromaddress`.
