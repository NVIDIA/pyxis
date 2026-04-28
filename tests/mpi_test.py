#!/usr/bin/env python3
import os
import socket
import sys
import traceback


def fail(rank, stage, exc):
    print(f"[rank {rank} on {socket.gethostname()}] FAIL at {stage}: {exc!r}",
          flush=True)
    traceback.print_exc()
    sys.exit(1)


def main():
    try:
        from mpi4py import MPI
    except Exception as e:
        print(f"FAIL: cannot import mpi4py: {e!r}", flush=True)
        sys.exit(1)

    comm = MPI.COMM_WORLD
    rank = comm.Get_rank()
    size = comm.Get_size()
    host = socket.gethostname()

    if rank == 0:
        print(f"[rank 0] MPI library: {MPI.Get_library_version().splitlines()[0]}",
              flush=True)
        print(f"[rank 0] world size = {size}", flush=True)

    # 1. Barrier — synchronize all ranks.
    try:
        comm.Barrier()
    except Exception as e:
        fail(rank, "Barrier", e)

    # 2. Bcast — root sends a small object to all.
    try:
        data = comm.bcast({"k": "v", "from": 0} if rank == 0 else None, root=0)
        assert data == {"k": "v", "from": 0}, f"got {data!r}"
    except Exception as e:
        fail(rank, "bcast", e)

    # 3. Allreduce of integer ranks.
    try:
        total = comm.allreduce(rank, op=MPI.SUM)
        expected = size * (size - 1) // 2
        assert total == expected, f"got {total}, expected {expected}"
    except Exception as e:
        fail(rank, "allreduce", e)

    # 4. Allgather — every rank receives all hostnames.
    try:
        all_hosts = comm.allgather(host)
        assert len(all_hosts) == size, f"got {len(all_hosts)} hosts"
    except Exception as e:
        fail(rank, "allgather", e)

    # 5. Point-to-point send/recv. Rank 0 pings every other rank in turn.
    try:
        if rank == 0:
            for dest in range(1, size):
                comm.send({"ping": dest}, dest=dest)
                pong = comm.recv(source=dest)
                assert pong == {"pong": dest}, f"rank 0 got {pong!r} from {dest}"
        else:
            msg = comm.recv(source=0)
            assert msg == {"ping": rank}, f"rank {rank} got {msg!r}"
            comm.send({"pong": rank}, dest=0)
    except Exception as e:
        fail(rank, "send/recv", e)

    # 6. Allreduce of a numpy array.
    try:
        import numpy as np
        buf = np.full(1024, rank, dtype=np.int64)
        out = np.zeros_like(buf)
        comm.Allreduce(buf, out, op=MPI.SUM)
        expected = size * (size - 1) // 2
        assert (out == expected).all(), \
            f"got out[0..3]={out[:4]}, expected {expected}"
    except ImportError:
        if rank == 0:
            print("[rank 0] numpy not available, skipping array Allreduce",
                  flush=True)
    except Exception as e:
        fail(rank, "numpy Allreduce", e)

    # 7. Final barrier so all ranks confirm completion.
    try:
        comm.Barrier()
    except Exception as e:
        fail(rank, "final Barrier", e)

    if rank == 0:
        print(f"[rank 0] PASS — all stages OK across {size} ranks",
              flush=True)


if __name__ == "__main__":
    main()
