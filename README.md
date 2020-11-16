### Deprecated

Superseded by https://github.com/etclabscore/go-etchash

# Etchash

Etchash is a modified version of ethash, recalibrated via ECIP-1099:
https://github.com/ethereumclassic/ECIPs/blob/master/_specs/ecip-1099.md

For details on etchash, please see the Ethereum wiki:
https://github.com/ethereum/wiki/wiki/Ethash

### Notes

ubuntu:
`apt-get install libboost-all-dev`

### Coding Style for C++ code:

Follow the same exact style as in [cpp-ethereum](https://github.com/ethereum/aleth/blob/master/CODING_STYLE.md)

### Coding Style for C code:

The main thing above all is code consistency.

- Tabs for indentation. A tab is 4 spaces
- Try to stick to the [K&R](http://en.wikipedia.org/wiki/Indent_style#K.26R_style),
  especially for the C code.
- Keep the line lengths reasonable. No hard limit on 80 characters but don't go further
  than 110. Some people work with multiple buffers next to each other.
  Make them like you :)
