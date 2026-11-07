# Volunteer briefing

## Read this to the volunteers (about one minute)

> We're teaching WiFi to work out where people are in this room, with no cameras. The
> small boards on stands send radio signals across the room, and your body changes those
> signals. You'll stand and walk in taped squares while the computer records, so it learns
> what each square looks like.
>
> 1. **Don't touch, bump or move the boards, their stands or their cables.** If you knock
>    one, tell us, even slightly. Moving a board means redoing the recording.
> 2. **Phones in airplane mode**, or WiFi off, and in your pocket. Take off backpacks and
>    big bags. Wear normal clothes.
> 3. **You have a colour: Blue, Orange or Green.** The screen and the voice tell each
>    colour which square to go to, for example "Blue, go to B2".
> 4. **"Walk to your square"**: walk straight there at a normal pace.
> 5. **"Stand still"**: stand near the middle of your square with your arms relaxed.
>    Breathe normally and face any direction. Please don't talk, wave or lean on anything.
> 6. **"Walk around inside your square"**: walk slowly but keep moving, and stay inside
>    the tape.
> 7. **"Everyone out of the room"**: leave completely. Don't stand in the doorway, and
>    stay out until you're called back. The first and last minute of every session are an
>    empty room.
> 8. Nobody else comes into the room during a session. Watchers stay outside.
>
> A session takes about 10–20 minutes. We take breaks between sessions.

## Setting up the room (do this once)

**Tape a 3 × 3 grid on the floor.** Each square is about 6 ft 8 in (2.03 m) on a side.

- **Rows are numbers.** Row **1** is along the wall with the S3 receiver (called "south"
  in `room.json`), and row 3 is the far wall.
- **Columns are letters.** **A** is on the left when you stand at the receiver wall
  facing into the room, then B, then C.
- **Label each square** with tape in its middle: A1 to C3. The `/collect` page shows the
  same map.

```
              north wall (TX 2 - TX 3 - TX 4)
        +-----------+-----------+-----------+
        |    A3     |    B3     |    C3     |
  TX 1  +-----------+-----------+-----------+
  west  |    A2     |    B2     |    C2     |  TX 5 east
        +-----------+-----------+-----------+
        |    A1     |    B1     |    C1     |
        +-----------+-----------+-----------+
              south wall (S3 receiver in the middle, AP in the corner)
```

**Place the boards.**

- **Height:** every board at the **same height, about 1 m** (chest height). Use stands,
  shelves or tape on the wall.
- **S3 antenna:** mount it **vertically**.
- **Positions:** the defaults in `pi_server/room.json` match the picture above. Measure
  where each board really ends up (±10 cm) and correct `room.json`. Then restart the
  server: `sudo systemctl restart csi-server`.
- **Once you start recording, never move a board.** Number each one with tape (TX 1 =
  `NODE_ID 1`, and so on).

**Give each volunteer their colour:** a sticker or a bit of coloured tape on the shirt.

**Show the `/collect` page on a screen everyone can see,** with the volume up so they can
hear the instructions. The device showing it should be on the WiFi's **5 GHz** band, or
on the Pi's cable, so it doesn't add traffic on the 2.4 GHz sensing channel.

## Session plan (about 1.5 hours with breaks)

Open `http://csi-pi.local:5000/collect`, choose the settings and press **Start**. The
first empty-room step calibrates automatically.

| # | Volunteers in room | Rounds | In each square | About |
|---|---|---|---|---|
| 1 | 1 (person A) | 2 | stand + walk | 17 min |
| 2 | 1 (person B, a different build) | 2 | stand + walk | 17 min |
| 3 | 2 (A + B) | 2 | stand | 9 min |
| 4 | 3 (A + B + C) | 2 | stand | 9 min |
| 5 | 1 (person C), the **test** session | 1 | stand + walk | 9 min |

The defaults are 20 s per square, 6 s to walk, and a 30 s empty room at the start.

**Then train, on the Pi:**

```bash
ssh csi-pi
cd ~/csi/pi_server
python3 train_model.py --list                       # see the session ids
python3 train_model.py --test-session <id of #5>    # honest score on unseen data
python3 train_model.py                              # final model from everything
curl -X POST localhost:5000/api/model/reload        # the 3D view switches to "AI model"
```

**Tips:**

- A session where the data status turned yellow or red (a transmitter dropped out) is
  worth redoing.
- The more different days and different people you record, the better it gets on new
  days.
- If the furniture or a door changes, record a new calibration. Re-train if it's a big
  change.
