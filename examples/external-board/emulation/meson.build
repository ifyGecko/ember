# Every source of the runner, one line each. A new device is a new file here and one
# create/map/connect block in the board file; nothing else changes.
board_sources = files(
  'main.c',        # registers the board, runs ember's stock command line
  'ext_board.c',   # the board: processor, memory, images, wiring. No device logic.
  'ext_id.c',      # one hardware component per file: the identification block
)
