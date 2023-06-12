# KomodoOcean (komodo-qt) 4 Marmara #

![Downloads](https://img.shields.io/github/downloads/DeckerSU/KomodoOcean/total)

This `static-mcl` branch is the GUI version of the `KomodoOcean` (`komodo-qt`) wallet for [Marmara Credit Loops](https://marmara.io/). It is experimental and currently supports builds only for Linux and Windows; MacOS builds are not tested and are not guaranteed. For build instructions, visit the project's [main](https://github.com/DeckerSU/KomodoOcean#readme) page.

## Chain Params ##

from Marmara Connector [sources](https://github.com/marmarachain/marmara-connector/blob/master/src/main/python/chain_args.py#L5):

```
[komodod|komodo-qt] -ac_name=MCL -ac_supply=2000000 -ac_cc=2 -addnode=161.97.146.150 -addnode=5.189.149.242 -addnode=149.202.158.145 -addressindex=1 -spentindex=1 -ac_marmara=1 -ac_staked=75 -ac_reward=3000000000 -pubkey=%your_pubkey% &
```

This version of the wallet (node) has optimized memory usage, so a fully synced node takes only ~1.7 Gb of RAM.