# Chain Analysis: `getaddressactivity` RPC

## Overview

`getaddressactivity` is an asynchronous RPC that performs a full chain scan and
collects per-address activity statistics for every transparent (t-) address that
has ever appeared on the KMD chain. The goal is to show address *activity*, not
just balances.

For each address the following data is collected:

- address
- current balance
- balance bucket: `<1`, `1-10`, `10-100`, `100-1k`, `1k-10k`, `10k-100k`, `100k+` (KMD)
- first transaction date
- last transaction date
- number of transactions in the last 30 / 90 / 365 days
- lifetime number of transactions
- known label if available: `notary`, `technical`, `unknown`

Because the resulting table can contain millions of rows, the full per-address
data is exported as a **CSV file** in the daemon data directory
(`addressactivity-<height>.csv`), while the RPC operation result contains only a
summary (totals, bucket/label distribution and the path to the file).

The RPC is implemented as an `AsyncRPCOperation`
(`AsyncRPCOperation_getaddressactivity` in `src/rpc/blockchain.cpp`), so it does
not block the RPC server: the call returns an operation id immediately, the scan
runs on the async RPC worker thread, and progress can be tracked with the
standard `z_getoperationstatus` / `z_getoperationresult` calls.

## Usage

```shell
# start the scan (all addresses, including zero-balance ones)
komodo-cli getaddressactivity
opid-12c01c2a-2b9f-4a1e-9387-2c0c1f2a7b8e

# start the scan, but only export addresses with current balance >= 1 KMD
komodo-cli getaddressactivity 1.0

# track progress
komodo-cli z_getoperationstatus '["opid-12c01c2a-2b9f-4a1e-9387-2c0c1f2a7b8e"]'

# retrieve the summary once the operation has finished
komodo-cli z_getoperationresult '["opid-12c01c2a-2b9f-4a1e-9387-2c0c1f2a7b8e"]'
```

### Arguments

| # | Name          | Type    | Required | Default | Description                                                                  |
|---|---------------|---------|----------|---------|------------------------------------------------------------------------------|
| 1 | `min_balance` | numeric | no       | `0`     | Only export addresses with current balance >= `min_balance` (in KMD) to the CSV file. With the default of `0` every address ever active on chain is exported, including addresses whose current balance is zero. |

### Restrictions

- Can be used **only on the KMD chain itself** (not on assetchains started with
  `-ac_name`).

## Progress reporting

`AsyncRPCOperation_getaddressactivity::getStatus()` extends the default
operation status object with scan progress:

```json
{
  "id": "opid-12c01c2a-2b9f-4a1e-9387-2c0c1f2a7b8e",
  "status": "executing",
  "creation_time": 1765360000,
  "method": "getaddressactivity",
  "currentblock": 1234567,
  "begin_height": 1,
  "end_height": 4000000,
  "addresses_seen": 2150000
}
```

- `currentblock` — height currently being processed (the scan goes forward,
  from genesis to the tip);
- `begin_height` / `end_height` — scan range, fixed at the moment the
  operation is created;
- `addresses_seen` — number of unique addresses discovered so far.

The operation honours both `z_getoperationstatus`-style cancellation and daemon
shutdown requests: the scan loop checks `isCancelled()` and
`ShutdownRequested()` on every block and aborts cleanly.

## Result (summary)

On success, `z_getoperationresult` returns a summary like:

```json
{
  "elapsed_ms": 5421337.5,
  "begin_height": 1,
  "end_height": 4000000,
  "tip_time": 1765360000,
  "blocks_processed": 4000000,
  "txes_processed": 98765432,
  "unattributed_outputs": 123456,
  "addresses_total": 2345678,
  "min_balance": "0.00",
  "addresses_written": 2345678,
  "file": "/home/user/.komodo/addressactivity-4000000.csv",
  "balance_buckets": {
    "<1": 1800000,
    "1-10": 300000,
    "10-100": 150000,
    "100-1k": 70000,
    "1k-10k": 20000,
    "10k-100k": 5000,
    "100k+": 678
  },
  "labels": {
    "notary": 512,
    "technical": 1,
    "unknown": 2345165
  }
}
```

- `unattributed_outputs` — number of outputs whose destination could not be
  extracted (bare multisig, CryptoConditions outputs, etc.); these are not
  attributed to any address;
- `balance_buckets` / `labels` — distribution over the rows actually written
  to the CSV file (i.e. after the `min_balance` filter).

## CSV file format

The file is written to the data directory as `addressactivity-<end_height>.csv`,
one row per address:

```csv
address,balance,balance_bucket,first_tx_date,last_tx_date,tx_count_30d,tx_count_90d,tx_count_365d,tx_count_lifetime,label
RXL3YXG2ceaB6C5hfJcN4fvmLH2C34knhA,12345.67890000,10k-100k,2016-09-13,2026-06-10,123,456,7890,1234567,technical
RVNKRr2fxF9veJVxsbAvuPPBhAjGAaTBUy,1.00000000,1-10,2018-01-15,2024-03-02,0,0,0,42,notary
...
```

| Column               | Description                                                              |
|----------------------|--------------------------------------------------------------------------|
| `address`            | Transparent KMD address                                                  |
| `balance`            | Current balance in KMD at `end_height`                                   |
| `balance_bucket`     | `<1`, `1-10`, `10-100`, `100-1k`, `1k-10k`, `10k-100k`, `100k+`          |
| `first_tx_date`      | Date (UTC, `YYYY-MM-DD`) of the first transaction involving the address  |
| `last_tx_date`       | Date (UTC, `YYYY-MM-DD`) of the last transaction involving the address   |
| `tx_count_30d`       | Number of transactions in the last 30 days                               |
| `tx_count_90d`       | Number of transactions in the last 90 days                               |
| `tx_count_365d`      | Number of transactions in the last 365 days                              |
| `tx_count_lifetime`  | Lifetime number of transactions                                          |
| `label`              | `notary`, `technical` or `unknown`                                       |

## Methodology

### Balance computation

The scan walks the chain **forward** (genesis → tip) and maintains an in-memory
map `outpoint → (address, amount)` of currently unspent outputs:

- every transaction output with an extractable destination credits the
  receiving address and inserts the outpoint into the map;
- every transaction input looks up its prevout in the map, debits the sending
  address and erases the entry.

The resulting per-address balance is therefore the sum of its unspent outputs
at `end_height`. KMD interest is handled naturally by this accounting (interest
appears as output value exceeding input value).

### Transaction counting

A transaction is counted **once per participating address**, regardless of how
many inputs/outputs of that transaction touch the address. An address
"participates" in a transaction if it receives at least one output or spends at
least one input in it.

### Time windows

The 30 / 90 / 365-day windows are counted back from the **timestamp of the
chain tip at the moment the scan starts** (`tip_time` in the summary), not from
wall-clock time. This makes the results reproducible on any chain snapshot.

### Address labels

Currently only two label sources are available in the daemon:

- `notary` — addresses derived from the notary node pubkeys of **all** KMD
  seasons (`notaries_elected` table). Since a P2PK script and a P2PKH script
  for the same pubkey encode to the same address string, both coinbase (P2PK)
  and regular (P2PKH) notary outputs are covered;
- `technical` — the crypto777 address (derived from `CRYPTO777_PUBSECPSTR`),
  used as the destination of notarisation transactions.

Other categories from the original request (exchange, treasury, team, burn)
require external curated address lists and are reported as `unknown` for now;
the label map in the implementation is a single lookup table and can easily be
extended once such lists are available.

### What is not covered

- **Shielded (z-) transactions** — value inside the Sprout/Sapling pools is not
  attributable to addresses by design and is ignored;
- **Non-standard outputs** — outputs whose destination cannot be extracted
  (bare multisig, CryptoConditions, etc.) are skipped and counted in
  `unattributed_outputs`. Consequently, spends of such outputs are not
  attributed either, so the accounting stays consistent.

## Performance and resource usage

The scan reads every block from disk and keeps two large in-memory structures:

- the unspent-output map (approximately the size of the UTXO set at the
  currently scanned height);
- the per-address statistics table (one entry per address ever active).

On the full KMD chain expect the scan to take a long time and to consume
several GB of RAM. Running it on a node with spare memory is recommended.

## Files changed

- `src/rpc/blockchain.cpp` — `AsyncRPCOperation_getaddressactivity` and the
  `getaddressactivity` RPC handler, command table registration;
- `src/rpc/server.h` — RPC handler declaration;
- `src/rpc/client.cpp` — client-side parameter conversion (`min_balance` as
  numeric).
