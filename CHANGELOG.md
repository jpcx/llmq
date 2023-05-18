# [0.2.0](https://github.com/jpcx/llmq/tree/0.2.0) 2023-05-17

## Fixed

- Compound context paths now work properly
- gpt plugin now works with immediate EOF from stdin
- Touched up various sections of the README

## Changed

- rm renamed to del for clear

## Added

- Added temporary context support
- Added several more demos and example scripts
- Added onfinish listener to plugin base class

# [0.1.2](https://github.com/jpcx/llmq/tree/0.1.2) 2023-04-26

## Changed

- plugins/\*.mk files included after `all:` to allow for target-specific variables.

## Added

- Added an example for using \*.mk files.

# [0.1.1](https://github.com/jpcx/llmq/tree/0.1.1) 2023-04-26

## Fixed

- Makefile now uses soft include to allow build without custom \*.mk files.

## Changed

- README edits.

# [0.1.0](https://github.com/jpcx/llmq/tree/0.1.0) 2023-04-25

## Added

- First release.
- gpt plugin added.
