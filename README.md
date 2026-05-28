# RNIC

UCX-based prototype for accelerating model-weight distribution by multiplexing
ordinary NIC and RNIC/RDMA paths.

## Prototype

The first prototype transfers one file from `4090-2` to `Cambricon-1`.
The sender splits the file into fixed-size chunks, assigns chunks to one or more
UCX stream lanes, and the receiver reconstructs the file by offset. Each chunk
has an FNV-1a checksum for fast in-flight validation, and the complete file is
verified with SHA256 after transfer.

Default test topology:

- Sender: `4090-2`, workspace `~/zhangzhisen/RNIC`
- Receiver: `Cambricon-1`, workspace `~/zhangzhisen/RNIC`
- Ordinary NIC path: `10.102.0.232 -> 10.102.0.239`
- RDMA path: `192.168.2.243 -> 192.168.2.248`
- UCX root: `/usr/local/ucx`

## Build

```bash
cmake -S . -B build -DUCX_ROOT=/usr/local/ucx -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

or on either server:

```bash
./scripts/build_remote.sh ~/zhangzhisen/RNIC
```

## Run

Start the receiver on `Cambricon-1` first:

```bash
MODE=hybrid OUT_FILE=~/zhangzhisen/rnic_received.bin ./scripts/run_receiver.sh
```

Use `SINK=1` on the receiver to measure receive/network overhead without
writing the reconstructed file to disk:

```bash
SINK=1 MODE=rdma-only ./scripts/run_receiver.sh
```

Then start the sender on `4090-2`:

```bash
MODE=hybrid IN_FILE=/path/to/model-weight.bin ./scripts/run_sender.sh
```

Supported modes:

- `nic-only`: UCX TCP lane on the ordinary NIC path.
- `rdma-only`: UCX RDMA lane on the RNIC path. The prototype uses a small
  POSIX TCP control socket only to exchange UCP worker addresses; the UCX lane
  itself uses `rc_mlx5` over `mlx5_0:1`, with `ud_mlx5` as the RDMA auxiliary
  wireup transport required by this UCX build.
- `hybrid`: UCX TCP + RDMA lanes, with static 30/70 chunk weighting.

Run all three modes from `4090-2` after both servers have the repository:

```bash
IN_FILE=/path/to/model-weight.bin ./scripts/run_three_modes.sh
```

JSON results are written under `results/`.

## Environment Checks

```bash
/usr/local/ucx/bin/ucx_info -d
ibv_devinfo -l
ibdev2netdev
ping 10.102.0.239
ping 192.168.2.248
```
