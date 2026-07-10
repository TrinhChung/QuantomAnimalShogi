# Quantum Animal Shogi — Source-Code Rule Specification

Version: 2026-07-10  
Status: source-code-derived rule specification  
Scope: game rules only. This document does not define engine search algorithms, EAQS optimization, alpha-beta, transposition tables, or performance policy.

---

## 1. Executive summary

This project implements Quantum Animal Shogi as a deterministic public-state game with quantum-like identity uncertainty represented by per-token role masks.

The implemented rule model is:

- Legal movement is **existential**: a board move is legal if at least one currently possible role of the source token can make that move.
- After a selected board move, impossible roles are removed from the moving token, then global identity propagation is applied.
- Catch/Lion capture is based on a **possible-Lion bit after transition disambiguation**, not on a forced singleton Lion mask.
- Try is also based on a **possible-Lion bit**, not forced Lion, and is delayed until the opponent has made one response action.
- Chick promotion is automatic when a board move ends on the far rank.
- Hen is represented as a promoted movement role bit, but global identity propagation normalizes Hen back to Chick/Bird lineage.
- Captured Hen demotes to Chick.
- Drops do not collapse identity and do not trigger promotion.
- Draw is only the 256-action limit. Repetition draw is not implemented.
- Illegal action at the Python/PettingZoo boundary is an immediate loss for the acting player.
- Terminal priority at the Python/PettingZoo boundary is Try, then Catch, then draw.
- The raw action space has 240 actions: 20 source selectors times 12 destination squares.

---

## 2. Coordinate and orientation model

The core uses a rotating board orientation. After each accepted action:

1. token ownership bits are complemented so the opponent becomes the side to move;
2. all board bitboards are rotated by 180 degrees;
3. the turn counter is incremented.

Therefore, in the core’s canonical side-to-move orientation, the current player’s far rank is always represented by squares `9, 10, 11`.

The Python raw action boundary uses Python coordinates and then converts to the core coordinates.

For raw action `a`:

```text
q = a // 12
r = a % 12
```

- `q = 0..11`: board source square in Python coordinate system.
- `q = 12..19`: hand-token ordinal source selector.
- `r = 0..11`: board destination square in Python coordinate system.

Conversion:

```text
board move: core action = (11 - q, 11 - r)
drop:       core action = (q,      11 - r)
```

---

## 3. Role-bit encoding

Each physical token has a role mask. The source implementation uses five movement bits:

| Role | Bit | Hex | Meaning |
|---|---:|---:|---|
| Chick | 0 | `0x01` | unpromoted Bird-lineage movement |
| Giraffe | 1 | `0x02` | orthogonal one-step movement |
| Elephant | 2 | `0x04` | diagonal one-step movement |
| Lion | 3 | `0x08` | king-like one-step movement |
| Hen | 4 | `0x10` | promoted Chick movement |

Important semantic point:

- Hen is not treated as an independent fifth underlying identity for global uniqueness.
- During global propagation, Hen is normalized to Chick/Bird lineage.
- The practical state mask may contain combinations such as `{Hen, Giraffe}`. This means: if the token is Bird-lineage, it is currently promoted; if it is Giraffe, the Hen bit is irrelevant after normalization.

Useful constants:

```text
CHICK   = 0x01
GIRAFFE = 0x02
ELEPHANT= 0x04
LION    = 0x08
HEN     = 0x10
BASE_MASK = CHICK | GIRAFFE | ELEPHANT | LION  // 0x0f
ANY_MASK  = CHICK | GIRAFFE | ELEPHANT | LION | HEN // 0x1f
```

Hen normalization used for identity-count propagation:

```text
normalized = (mask | (mask >> 4)) & 0x0f
```

This maps `Hen` into the Chick/Bird slot.

---

## 4. State fields required by the rules

A complete game state must contain at least:

```text
pieces[8]      // role mask for each physical token
ownership      // owner of each physical token, represented as bits
bit_boards[8]  // one board bit per board token, or 0 if token is in hand
turn           // number of accepted actions so far
```

Derived concepts:

- A token is in hand iff `bit_boards[i] == 0`.
- A token is on board iff `bit_boards[i] != 0`.
- The side-to-move is represented implicitly by the rotating board and complemented ownership model.
- The original four-token groups are token indices `0..3` and `4..7`. Global propagation runs independently over these two original groups.

No repetition history, repeated-position counter, Zobrist repetition key, or path-history field exists in the implemented rules.

---

## 5. Legal movement rule

### 5.1 Board moves

A board move is legal if all of the following hold:

1. The source selects a current-player token on the board.
2. The destination is not occupied by a friendly token.
3. At least one possible role in the source token’s current mask can move from source to destination.

This is existential legality:

```text
legal(source, destination)
    iff exists role in pieces[token] such that role can move source -> destination
```

Example:

```text
pieces[p] = CHICK | GIRAFFE
move = sideways
```

This move is legal because Giraffe can move sideways even though Chick cannot.

### 5.2 Movement collapse

After a legal board move is selected, the moving token’s mask is filtered to the roles that can perform that exact source-to-destination move.

Example:

```text
before: pieces[p] = CHICK | GIRAFFE
action: sideways move
after local movement filter: pieces[p] = GIRAFFE
```

After this local filter, global identity propagation is run.

### 5.3 Drops

A drop is legal if:

1. the source selector names an available current-player hand token by ordinal;
2. the destination square is empty.

Drops do not require a movement role. Drops use no piece-type action field.

A drop does not collapse identity and does not trigger promotion, even if the destination is the far rank.

---

## 6. Global identity propagation

After movement filtering and after specific capture/Catch-disambiguation updates, global propagation is applied.

Propagation runs independently over the two original token groups:

```text
group A = tokens 0..3
group B = tokens 4..7
```

For propagation, Hen is normalized to Chick:

```text
normalized = (mask | (mask >> 4)) & 0x0f
```

The uniqueness constraint is over four base identity slots:

```text
Chick/Bird, Giraffe, Elephant, Lion
```

The implementation uses a closed-subset/Hall-set style propagation:

- If a subset of tokens can only occupy exactly the same number of base identity slots as the subset size, those base identities are removed from the remaining tokens in the same original group.
- This is repeated until a fixed point.

Propagation is exact for the implemented bitmask approximation but does not explicitly store full assignment sets.

---

## 7. Capture and demotion

When a board move lands on an occupied destination square, the destination occupant is captured before the mover’s movement mask is filtered.

Capture update:

1. The captured token’s Hen bit is demoted to Chick:

```text
captured_mask = (captured_mask | (captured_mask >> 4)) & 0x0f
```

2. The captured token’s ownership changes to the mover.
3. The captured token’s board bit is set to `0`, making it a hand token.

Captured hand tokens are therefore unpromoted after legal capture.

---

## 8. Promotion rule

Promotion is automatic and has no action flag.

For a board move, after movement filtering and the first global propagation, if the move destination is the far rank:

```text
destination in {9, 10, 11}
```

then any remaining Chick possibility is promoted into Hen.

Promotion transform:

```text
if destination in far rank:
    mask = (mask | (mask << 4)) & 0x1e
```

Consequences:

- `CHICK` becomes `HEN`.
- A mixed token such as `{Chick, Giraffe}` may become `{Hen, Giraffe}` if both possibilities survive before promotion.
- A non-Bird token is unchanged by promotion.
- Promotion is not optional.
- A Hen can later leave the far rank and remains Hen until captured.
- A hand drop onto the far rank does not promote.

---

## 9. Catch / Lion capture rule

Catch is not checked immediately at the raw capture moment. It is checked after the full source-defined transition update and board rotation.

The source-code transition includes a special captured-Lion disambiguation step before terminal Catch check.

After the mover has captured a token and after movement filtering, global propagation, promotion, and placement:

1. If any enemy-owned token still has Lion possibility, then Lion is removed from every current-player hand token.
2. Global propagation runs again.
3. Later, after side switch and board rotation, terminal predicates are checked.

Catch terminal predicate:

```text
Catch is true if the previous mover owns at least one hand token whose mask contains Lion.
```

Equivalently, from the relevant post-transition perspective:

```text
exists hand token p owned by the catching side such that (pieces[p] & LION) != 0
```

A singleton Lion mask is not required.

Important cases:

### Case 1: captured token was possible-Lion, but another enemy-owned token can still be Lion

The captured hand token loses the Lion bit during disambiguation. No Catch win occurs.

### Case 2: captured token was possible-Lion, and no enemy-owned token can still be Lion

The captured hand token retains the Lion bit. Catch win occurs.

### Case 3: direct call to the Catch predicate on a hand token `{Lion, Elephant}`

The predicate treats this as Catch because it tests possible-Lion bit membership, not singleton certainty. Normal legal transition disambiguation usually removes impossible Lion bits first.

---

## 10. Try rule

Try is based on possible-Lion, not forced-Lion.

Core Try predicate:

```text
Try is true if the side-to-move has any on-board token on squares 9..11 whose mask contains Lion.
```

Equivalent:

```text
exists token p owned by side_to_move:
    bit_boards[p] intersects far_rank_mask
    and (pieces[p] & LION) != 0
```

The far rank in the core rotating coordinate system is always:

```text
{9, 10, 11}
```

### Delayed Try timing

Try does not win immediately when a player moves a possible-Lion into the far rank.

Timing:

1. Player A moves a possible-Lion token into A’s far rank.
2. The board rotates and Player B becomes side to move.
3. Player B gets one ordinary response action.
4. If Player B captures or otherwise removes the possible-Lion threat, Try does not occur.
5. If after Player B’s response and the subsequent rotation Player A’s possible-Lion token is still on A’s far rank, the Try predicate fires and Player B loses.

There is no separate attack-map safety check. Safety is implemented by giving the opponent one actual response action.

---

## 11. Draw rule

Draw is only the 256-action limit.

```text
MAX_TURN = 256
Draw iff turn >= 256
```

There is no repetition draw.

The implemented state contains no repeated-position history and no repetition counter.

---

## 12. Terminal priority and rewards at Python/PettingZoo boundary

After an accepted action has been applied with `Game::next_state`, the Python/PettingZoo boundary checks terminal predicates in this order:

```text
1. Try
2. Catch
3. Draw
```

Terminal priority:

```text
Try overrides Catch if both predicates are true at the boundary check.
Catch overrides draw.
Try overrides draw.
```

Reward semantics at the acting-player boundary:

- If `Game::won` is true after the acting player’s move, the acting player loses. This corresponds to the opponent’s delayed Try having survived.
- If `Game::lost` is true after the acting player’s move, the acting player wins. This corresponds to the acting player having achieved Catch.
- If `Game::draw` is true, the action result is draw reward.

From the Python/PettingZoo report:

```text
if won(next_state):  return -1 to acting player
if lost(next_state): return +1 to acting player
if draw(next_state): return -0.5
otherwise:           return 0
```

The PettingZoo wrapper terminates both agents on terminal results and assigns the opposite reward to the opponent for win/loss cases.

---

## 13. Illegal action rule

At the Python/PettingZoo boundary:

1. the raw action is decoded;
2. legal actions are generated;
3. if the decoded action is not a member of the legal action set, the acting player immediately loses;
4. the state is not mutated;
5. both players are marked terminated by the wrapper;
6. the opponent receives the winning reward.

Illegal action behavior:

```text
illegal action => acting player reward -1
state unchanged
opponent reward +1 in PettingZoo wrapper
```

The core `next_state` function itself assumes the caller supplied a legal action and does not fully validate malformed actions. Normal match clients should validate against legal actions first.

---

## 14. Raw action space

The action space size is:

```text
20 sources * 12 destinations = 240 actions
```

Sources:

```text
0..11  = board source squares
12..19 = current-player hand-token ordinal selectors
```

Destinations:

```text
0..11 = board destination squares
```

No raw action field exists for:

- identity;
- piece type;
- promotion choice;
- capture flag;
- token id directly.

Hand source selectors choose the nth current-player hand token in internal token-index order.

Different raw hand actions may be observationally similar when hand tokens are indistinguishable by public mask and role, but internally they may still move different physical tokens.

---

## 15. Exact transition pipeline

For a Python/PettingZoo accepted action:

1. Decode raw action index into `(source, destination)`.
2. Convert Python coordinates to core coordinates.
3. Generate legal actions.
4. Validate action membership.
5. If illegal, return immediate loss and do not mutate state.
6. If `source < 12`, process as board move:
   1. Find destination occupant, if any.
   2. If occupied, demote captured Hen to Chick.
   3. If occupied, change captured token owner to mover.
   4. If occupied, set captured token board bit to `0`, moving it to hand.
   5. Find moving token at source.
   6. Filter moving token’s mask to identities capable of the exact source-to-destination move.
   7. Run global propagation over original token groups.
   8. If destination is far rank and Chick remains, promote Chick to Hen.
   9. Set moving token board bit to destination.
   10. If any enemy-owned token still has Lion possibility, remove Lion from all current-player hand tokens.
   11. Run global propagation again.
7. Else, process as drop:
   1. Enumerate current-player hand tokens in increasing internal token index.
   2. Select ordinal `source - 12`.
   3. Set that token’s board bit to destination.
   4. Do not collapse, promote, or reveal identity.
8. Complement ownership.
9. Rotate every board bitboard by 180 degrees.
10. Increment turn.
11. Check Try predicate.
12. Check Catch predicate.
13. Check draw predicate.
14. If none applies, game continues.

---

## 16. Rule invariants

The following invariants should be maintained by legal play:

1. Each token has a non-empty role mask.
2. A token is either on exactly one board square or in hand.
3. Two tokens should not legally occupy the same board square.
4. Hand tokens have `bit_boards[i] == 0`.
5. Captured Hen is demoted to Chick.
6. Drops do not promote.
7. Global propagation normalizes Hen to Chick for base identity uniqueness.
8. Repetition is not part of terminal logic.
9. Terminal check order at the Python/PettingZoo boundary is Try, Catch, draw.
10. Core `next_state` expects legal input; external environments must enforce illegal-action loss.

---

## 17. Minimal rule test suite

These tests should be kept as source-project regression tests.

Role constants:

```text
C = 0x01
G = 0x02
E = 0x04
L = 0x08
H = 0x10
```

### 17.1 Existential movement

Setup: token mask `{C, G}` at a square where sideways movement is possible by Giraffe but not Chick.

Expected:

```text
move is legal
resulting moving-token mask is {G}
```

### 17.2 Capture possible-Lion but not Catch

Setup: captured token has `{L, E}`, but another enemy-owned token still has Lion possibility.

Expected:

```text
captured hand token loses Lion bit
Catch is false
game continues
```

### 17.3 Capture possible-Lion and Catch

Setup: captured token has Lion possibility and no remaining enemy-owned token can still be Lion after propagation/disambiguation.

Expected:

```text
captured hand token keeps Lion bit
Catch is true
acting player wins at Python/PettingZoo boundary
```

### 17.4 Possible-Lion enters Try zone

Setup: token with mask `{L, G}` moves to far rank.

Expected:

```text
no immediate Try win on the entry move
opponent receives one response action
if token survives, Try fires after opponent response
```

### 17.5 Forced-Lion enters Try zone

Setup: token with mask `{L}` moves to far rank.

Expected:

```text
same delayed timing as possible-Lion Try
```

### 17.6 Try token captured during response

Setup: possible-Lion or forced-Lion enters far rank; opponent has a legal response that captures it.

Expected:

```text
responder’s capture prevents Try
Catch may be awarded if captured token retains Lion bit according to Catch rule
```

### 17.7 Chick promotion

Setup: Chick moves to far rank.

Expected:

```text
mask becomes {H}
```

### 17.8 Mixed promotion

Setup: mixed `{C, G}` reaches far rank through a legal move that preserves both possibilities before promotion.

Expected:

```text
mask becomes {H, G}
```

### 17.9 Non-Bird far-rank move

Setup: Giraffe moves to far rank.

Expected:

```text
mask remains {G}
```

### 17.10 Hen capture/demotion

Setup: capture a token with mask `{H}`.

Expected:

```text
captured token enters hand
owner changes
mask becomes {C}
```

### 17.11 Hand drop

Setup: drop an uncertain hand token onto far rank.

Expected:

```text
mask unchanged
no promotion
no movement collapse
```

### 17.12 Turn-limit draw

Setup: nonterminal state with `turn = 255`; perform a nonterminal legal action.

Expected:

```text
turn becomes 256
draw reward applies
```

### 17.13 Final-action Catch priority

Setup: Catch-producing action with `turn = 255`.

Expected:

```text
Catch win overrides draw
```

### 17.14 Final-action Try priority

Setup: delayed-Try predicate becomes true with `turn = 255`.

Expected:

```text
Try result overrides draw
```

### 17.15 Repetition is ignored

Setup: repeat a previous state configuration with `turn < 256`.

Expected:

```text
no draw occurs solely from repetition
```

### 17.16 Illegal raw action

Setup: submit a raw action not in legal actions.

Expected:

```text
acting player reward = -1
state unchanged
both players terminated in PettingZoo wrapper
opponent reward = +1
```

---

## 18. Non-rules and explicitly unsupported behavior

The following are not implemented as game rules:

- no repetition draw;
- no optional promotion;
- no promotion action flag;
- no immediate Try win on entry;
- no explicit attack-map Try safety check;
- no universal legal-move requirement;
- no forced-singleton Lion requirement for Catch or Try;
- no identity choice by the player;
- no random collapse;
- no probabilistic reward;
- no legal action recovery or fallback after illegal action at the Python/PettingZoo boundary.

---

## 19. Source-compatible formal summary

Let each token `p` have a public role mask `M[p]` over:

```text
{C, G, E, L, H}
```

Legal board move:

```text
a = (src, dst) is legal for token p
iff exists role r in M[p] such that r can move src -> dst
```

Movement collapse:

```text
M[p] := { r in M[p] | r can move src -> dst }
```

Promotion:

```text
if dst in {9,10,11}:
    C possibility becomes H
```

Capture demotion:

```text
H possibility becomes C for captured token
```

Propagation lineage normalization:

```text
lineage_mask(M) = (M | (M >> 4)) & 0x0f
```

Catch:

```text
After capture disambiguation and propagation,
Catch iff a hand token owned by the capturing side still has L in its mask.
```

Try:

```text
After opponent response and board rotation,
Try iff the side-to-move has an on-board token on {9,10,11} with L in its mask.
```

Draw:

```text
turn >= 256
```

Terminal priority:

```text
Try > Catch > Draw
```

Illegal action at match boundary:

```text
illegal => acting player loses, state unchanged
```
