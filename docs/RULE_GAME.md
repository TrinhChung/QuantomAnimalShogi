# Quantum Animal Shogi: Game Rules and Protocol

This document defines the game rules, contest protocol, action encoding, and runtime requirements. Search algorithms such as alpha-beta, transposition tables, and `L_eq` are documented separately.

## 1. Game Overview

Quantum Animal Shogi is a finite, deterministic, two-player, zero-sum, perfect-information game.

### 1.1 Board

The board contains 12 squares arranged in 3 columns and 4 ranks. The current player's side is always displayed at the bottom.

| Index | Coordinate |
|---:|:---:|
| 0 | 3一 |
| 1 | 2一 |
| 2 | 1一 |
| 3 | 3二 |
| 4 | 2二 |
| 5 | 1二 |
| 6 | 3三 |
| 7 | 2三 |
| 8 | 1三 |
| 9 | 3四 |
| 10 | 2四 |
| 11 | 1四 |

For the second player, the board is viewed from that player's perspective. Therefore, `3一` and `1四` are reversed relative to the first-player view.

### 1.2 Pieces and Movement

There are four base piece types. A Chick can promote to a Hen.

| Piece | Legal movement |
|---|---|
| Lion | One square in any of the eight adjacent directions |
| Elephant | One square diagonally in any of four directions |
| Giraffe | One square vertically or horizontally in any of four directions |
| Chick | One square forward |
| Hen | One square forward, backward, left, right, forward-left, or forward-right; never diagonally backward |

A Chick promotes to a Hen when it reaches the opponent's first rank.

### 1.3 Turn Progression

On each turn, the current player must perform exactly one legal action:

1. Move one owned board piece to a legal destination.
2. Drop one owned hand piece onto an empty board square.

Passing is not allowed. When a piece lands on an opponent's piece, it captures that piece and adds it to the current player's hand. A captured Hen becomes a Chick in hand.

### 1.4 Win and Draw Conditions

| Result | Condition |
|---|---|
| Catch | Capture the opponent's Lion. In the quantum game, capture the last piece that can still be the opponent's Lion. |
| Try | Move a piece that can be a Lion into the opponent's first rank. Try does not succeed if that Lion can be captured immediately under the game rules. |
| Illegal-action loss | A player sends an illegal action. |
| Draw | The turn count reaches 256. |

There is no repetition-draw rule. The following standard shogi restrictions do not apply:

- nifu (double pawn);
- pawn-drop mate;
- immobile-piece drop restrictions;
- prohibition against leaving the Lion in check.

## 2. Quantum State Rules

At the beginning of the game, each physical piece may have multiple possible identities. Each piece uses a five-bit possibility mask.

| Bit | Identity |
|---:|---|
| 0 | Chick |
| 1 | Giraffe |
| 2 | Elephant |
| 3 | Lion |
| 4 | Hen |

When a piece moves, remove every identity that could not make that move:

```text
new_mask = old_mask & move_possible_mask
```

For example, after a diagonal move, only Lion and Elephant remain possible.

### 2.1 Capture

- If the captured piece is not the final opponent Lion candidate, remove its Lion possibility.
- If the captured piece is the final opponent Lion candidate, Catch occurs and the game ends.

### 2.2 Global Count Constraints

For each origin side, exactly one piece belongs to each base lineage:

| Lineage | Required count |
|---|---:|
| Chick/Hen (`CH`) | 1 |
| Giraffe (`G`) | 1 |
| Elephant (`E`) | 1 |
| Lion (`L`) | 1 |

Chick and Hen are two states of the same original lineage, not separate count lineages.

After every move, run constraint propagation until it reaches a fixed point. If any piece mask becomes empty, the branch is invalid.

## 3. Communication Protocol

The engine exchanges one JSON value per line with the contest environment:

| Stream | Purpose |
|---|---|
| `stdin` | Receive JSON messages |
| `stdout` | Send protocol JSON only |
| `stderr` | Diagnostic logs only |

The engine must handle `get_action` and `end_game` commands.

## 4. `get_action` Input

Example input:

```json
{
  "command": "get_action",
  "observation": {
    "observation": [
      [0, 0, 0, 0, 0, 0, 0, 0, 0]
    ],
    "action_mask": [0, 1],
    "turn": 0
  }
}
```

The abbreviated arrays above illustrate their shape. Real input contains exactly 20 piece arrays and 240 action-mask integers.

### 4.1 Piece Entries

`observation.observation` contains 20 entries:

- indices `0..11`: board squares in the order defined in section 1.1;
- indices `12..19`: hand piece slots 1 through 8.

Each entry contains nine integers:

```text
[
  can_be_chick,
  can_be_giraffe,
  can_be_elephant,
  can_be_lion,
  can_be_hen,
  is_from_first_player,
  is_from_second_player,
  is_my_piece,
  is_opponent_piece
]
```

Each field is `1` when true and `0` otherwise. Empty board squares and unused hand slots contain no piece.

### 4.2 Action Mask

`action_mask` contains exactly 240 integers:

```text
action_mask[i] == 1  // action i is legal
action_mask[i] == 0  // action i is illegal
```

The engine may output only an action for which `action_mask[action] == 1`. For real environment input, the mask is the final legality authority.

Internal search must generate legal moves independently. It must not use an external `action_mask`; the mask is only for validating real environment input and output.

### 4.3 Turn Count

- The initial turn is `0`.
- The count increases by one after every action.
- The game is a draw when the count reaches `256`.

Internal state, hashing, and canonical keys must include the turn count because otherwise identical boards can have different values at different turns.

## 5. Action Encoding

An action is an integer in the inclusive range `0..239`:

```text
action = source_index * 12 + destination_index
```

Where:

- `source_index` `0..11` identifies a board square;
- `source_index` `12..19` identifies a hand slot;
- `destination_index` `0..11` identifies a board square.

Examples:

| Action | Source | Destination |
|---:|---:|---:|
| 0 | 0 | 0 |
| 11 | 0 | 11 |
| 12 | 1 | 0 |
| 143 | 11 | 11 |
| 144 | 12 | 0 |
| 239 | 19 | 11 |

Many encoded actions are impossible or illegal in a particular state. Always validate the chosen action.

## 6. `get_action` Output

Respond with exactly one JSON integer:

```json
123
```

The output must:

- be an integer in `0..239`;
- satisfy `action_mask[action] == 1`;
- contain no explanation or diagnostic text.

There is no pass action. Invalid JSON or an illegal action loses the game.

## 7. `end_game` Input and Output

Input:

```json
{
  "command": "end_game"
}
```

The engine may return any valid JSON value. Recommended response:

```json
"OK"
```

## 8. Reward

| Result | Reward |
|---|---:|
| Win | `1` |
| Loss | `-1` |
| Draw | `-0.5` |

## 9. Runtime Requirements

- Read input from `stdin` and write protocol output to `stdout`.
- Write logs only to `stderr`.
- Do not use a GPU or network access.
- Avoid paid libraries and excessive memory use.
- Approximate time limit: 30 seconds per action.
- After sending an action, do not consume CPU while waiting for new input.

## 10. Engine Integration Requirements

- Parse valid `get_action` and `end_game` messages.
- Build the internal `State` from the observation.
- Keep IO, rule, and search code separate.
- Represent internal `Move` independently from the external action integer.
- Implement action encode/decode in both directions.
- Include `turn` in `State`, its hash, and its canonical key.
- Validate the selected real-game action against `action_mask` before output.
- Never print debug text to `stdout`.

### Acceptance Tests

- Parse valid `get_action` input.
- Parse valid `end_game` input.
- Output a valid integer action.
- Never output an action whose mask entry is `0`.
- Encode/decode round trips for every action in `0..239`.
- Keep `stdout` restricted to protocol JSON.
- Permit diagnostic logs on `stderr`.
