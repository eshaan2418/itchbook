# vhs research/demo.tape  -> research/demo.gif
# Replays the 2019-01-30 NASDAQ ITCH sample from the open and prints AAPL's live book.
Output research/demo.gif
Set Shell "bash"
Set FontSize 15
Set Width 980
Set Height 520
Set Padding 16
Set Framerate 12
Set TypingSpeed 25ms
Hide
Type "export ITCH=$HOME/quantfactory/_data/itch/01302019.NASDAQ_ITCH50.gz && clear"
Enter
Show
Type "build/itchbook top $ITCH --symbol AAPL --from 09:30 --every-ms 200 --frames 45 --sleep-ms 110 --depth 6"
Sleep 500ms
Enter
Sleep 16s
