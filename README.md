# onion-match

`onion-match` is a command-line post-filter for generated `.onion` addresses.

It scans candidate addresses for **recognizable patterns after a chosen prefix**, helping you pick readable vanity addresses from large batches.

The tool is intended to work with vanity generators such as [mkp224o](https://github.com/cathugger/mkp224o). Instead of brute forcing a long name directly, you can generate many addresses with a short prefix and then filter them for readable onion addresses.

## Example Workflow

Generate candidate addresses with a short prefix:

``` bash
mkp224o -d onions alice
```

Filter them for readable patterns (where prefix.txt contains just `alice` and words_alpha.txt is a large wordlist like [dwyl/english-words](https://github.com/dwyl/english-words)):

``` bash
ls onions | ./onion-match prefix.txt words_alpha.txt
```

Example matches:

```
alicetechxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.onion
alicewowclub42xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.onion
```

Instead of manually inspecting thousands of similar addresses, the tool
returns only those containing recognizable segments.

## Build

``` bash
git clone https://github.com/jcwegman/onion-match.git
cd ./onion-match
make
```

or

``` bash
g++ -std=c++17 -O2 onion-match.cpp -o onion-match
```

## Usage

Basic filtering:

``` bash
ls onions | ./onion-match prefix.txt words_alpha.txt
# alicetechxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.onion
# alicecheesexxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.onion
# alicelolxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.onion
# ...
```

Control segment length:

``` bash
ls onions | ./onion-match prefix.txt words_alpha.txt --range=5-7
# alicecheesexxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.onion
# alicegainsxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.onion
# alicetrumpetxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.onion
# ...
```

Match multiple chained segments:

``` bash
ls onions | ./onion-match prefix.txt words_alpha.txt --range=5-7,3-5 --chain=2 --numbers
# alicecheeseboyxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.onion
# alicecheese7645xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.onion
# alicekoalamanxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.onion
# ...
```

Highlight matches in color:

``` bash
ls onions | ./onion-match prefix.txt words_alpha.txt --color=multi
```

Color output is automatically disabled when stdout is not a terminal, even for
`--color=yes` and `--color=multi`.

Insert separators between matched segments (useful when piping into another program):

``` bash
ls onions | ./onion-match prefix.txt words_alpha.txt --separator
# alice+lol+xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.onion
# ...
```

Input lines may include `.onion` or just the address portion.

For full option details:

``` bash
./onion-match --help
```
