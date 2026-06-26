# Data

Real market data drives the benchmark. No data is committed to this repository; the
`.gitignore` excludes every CSV under this directory. Place the files here yourself.

## The LOBSTER free academic sample

LOBSTER publishes a free academic sample that reconstructs the limit order book for a single
trading day from Nasdaq historical message data. Each sample is a pair of CSV files for one
instrument on one day:

- a message file, the event stream that drives the book,
- an orderbook file, the reconstructed top levels after every message, used by the
  correctness test as an external reference.

The two files share a name apart from the `message` versus `orderbook` token and a trailing
level count, for example `AAPL_2012-06-21_34200000_57600000_message_10.csv` and
`AAPL_2012-06-21_34200000_57600000_orderbook_10.csv`. Download the sample from the LOBSTER
website and drop both files into this directory.

## Message file columns

The message file has one event per row, with these columns in order:

1. Time, seconds after midnight, with decimals to nanosecond resolution.
2. Event type.
3. Order id.
4. Size, the number of shares.
5. Price, an integer in units of dollar times ten thousand. A price of 1234500 is 123.45
   dollars.
6. Direction, 1 for a buy limit order, minus 1 for a sell limit order.

Event types:

| Type | Meaning                                              | Effect on the book        |
|------|------------------------------------------------------|---------------------------|
| 1    | New limit order                                      | adds resting quantity     |
| 2    | Partial cancellation                                 | reduces a resting order   |
| 3    | Full deletion                                        | removes a resting order   |
| 4    | Execution of a visible limit order                   | reduces or removes resting|
| 5    | Execution of a hidden order                          | none, not on visible book |
| 7    | Trading halt indicator                               | none                      |

The direction on an execution and a cancellation refers to the side of the resting limit
order being affected, so the book recovers the side from the order id through its index and
does not rely on the direction field except when adding a new order.

## Orderbook file columns

The orderbook file has one row per message row, giving the state of the book after that
message. For an N level file each row has 4N columns, in repeating groups of four per level:
ask price, ask size, bid price, bid size, starting from the best level. Empty levels are
padded with a sentinel price and a size of zero. The correctness test compares the
reconstructed book against these rows level by level, skipping padded levels.

## Without data

If no data is present the benchmark falls back to a deterministic synthetic stream seeded
with 42, clearly labelled as synthetic, and the LOBSTER correctness test skips cleanly with
a message. Continuous integration runs without any data.
