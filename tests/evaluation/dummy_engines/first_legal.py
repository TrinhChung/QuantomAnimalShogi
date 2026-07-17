import json
import sys

for line in sys.stdin:
    message = json.loads(line)
    if message.get("command") == "end_game":
        print('"OK"', flush=True)
        continue
    mask = message["observation"]["action_mask"]
    print(next(index for index, enabled in enumerate(mask) if enabled), flush=True)
