# Copyright 2024 Bloomberg Finance L.P.
# Distributed under the terms of the Apache 2.0 license.
"""
Mostly-automated tests for bandwidth limit accuracy.

The intended usage is that you create a virtualenv, install `requirements.txt`, start up all of the docker containers
and then run these tests. Unfortunately the docker setup does not currently run haproxy, so you before running these
tests you will need to compile haproxy and then manually run *two* instances of it. The first using the existing
example config, and the other using the same config but listening on a port 100 greater than the default config.
You will also need to make a corresponding update to your local polygen config to point at both of those haproxy
instances (the example config only references one). Finally, you'll need to make sure that `strace` is installed
and available in your path.

After that, the following commands should work:

```
python3 -m virtualenv venv
. venv/bin/activate
pip install -r requirements.txt
docker-compose up
pytest --capture=tee-sys .
```

This should run all the tests, each of which should succeed and output a graph of data transfer.
The intention is that the correctness of these graphs is verified manually.
"""

import inspect
import os
import re
import statistics
import subprocess
import time
from collections.abc import Iterable, Sequence
from datetime import datetime, timedelta, timezone
from fcntl import F_GETFL, F_SETFL, fcntl
from typing import Optional, Tuple

import matplotlib.pyplot as plt  # type: ignore
from randsrv import parse_data_quantity

PORT = 9001

TransferDataPoint = Tuple[float, float]
TransferSeries = list[TransferDataPoint]


# We track time in "ticks" (that happen to be 1 second long here) because we need to smooth out the transfer
# graph somewhat to avoid it being just a rectangle of colour as the data transfer goes from 0 to the limit
# and back many, many times a second due to our limiting as well as just basic functioning of TCP.
def time2tick(t_seconds: float) -> int:
    return int(t_seconds * 1)


def tick2time(tick: int) -> float:
    return tick / time2tick(1)


def get_cmd(data: str, server: int, user: int) -> list[str]:
    return [
        "curl",
        f"http://localhost:{PORT + ((server - 1) * 100)}/{data}",
        "-H",
        f"Authorization: AWS user{user}key901234567890",
        "-o",
        "/dev/null",
    ]


def post_cmd(data_quantity: str, server: int, user: int) -> list[str]:
    datafile_dir = "/tmp/weir-qos-upload"
    if not os.path.isdir(datafile_dir):
        os.mkdir(datafile_dir)
    upload_path = os.path.join(datafile_dir, data_quantity)
    if not os.path.isfile(upload_path):
        num_bytes = parse_data_quantity(data_quantity)
        file_result = subprocess.run(
            f"head -c {num_bytes} /dev/urandom > {upload_path}", shell=True
        )
        file_result.check_returncode()

    return [
        "curl",
        f"http://localhost:{PORT + ((server - 1) * 100)}",
        "-H",
        f"Authorization: AWS user{user}key901234567890",
        "-o",
        "/dev/null",
        "--request",
        "POST",
        "--data-binary",
        f"@{upload_path}",
    ]


class BandwidthTracingProcess:
    process_handle: subprocess.Popen
    stdout_accumulator = ""
    stderr_accumulator = ""

    def __init__(self, cmd: list[str], label: "str | None" = None):
        self.label = label
        self.process_handle = subprocess.Popen(
            ["strace", "-f", "-e", "trace=network", "-ttt", "--"] + cmd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        # We passed PIPE for stdout and stderr so they're defined to not be None
        assert self.process_handle.stdout is not None
        assert self.process_handle.stderr is not None

        # We need to mark the stdout & stderr pipes as non-blocking so that reading from them will just
        # return an empty result instead of waiting for the next line of input. This ensures that we
        # don't hold up one process because another process (that we're always accumulating input from)
        # happens to not be writing anything to its output streams.
        stdout_flags = fcntl(self.process_handle.stdout, F_GETFL)
        fcntl(self.process_handle.stdout, F_SETFL, stdout_flags | os.O_NONBLOCK)
        stderr_flags = fcntl(self.process_handle.stderr, F_GETFL)
        fcntl(self.process_handle.stderr, F_SETFL, stderr_flags | os.O_NONBLOCK)

    def has_terminated(self) -> bool:
        return self.process_handle.poll() is not None

    def returncode(self) -> int:
        return self.process_handle.returncode

    def accumulate_output(self) -> None:
        # We passed PIPE for stdout and stderr so they're defined to not be None
        # Assert that here again to satisfy mypy
        assert self.process_handle.stdout is not None
        assert self.process_handle.stderr is not None

        # Bound the amount of output we read at a time so that we don't starve other processes
        for i in range(128):
            new_stdout = self.process_handle.stdout.readline()
            new_stderr = self.process_handle.stderr.readline()
            if len(new_stdout) == 0 and len(new_stderr) == 0:
                break
            self.stdout_accumulator += new_stdout
            self.stderr_accumulator += new_stderr

    def parse_network_stats(
        self, initial_timestamp: float | None = None
    ) -> "Tuple[TransferSeries, TransferSeries]":
        def burst2mbps(burst: float) -> float:
            burst_megabytes = burst / (1024 * 1024)
            bursts_per_second = time2tick(1)
            megabytes_per_second = burst_megabytes * bursts_per_second
            return megabytes_per_second

        assert self.has_terminated()
        if self.returncode() != 0:
            print(f"Process terminated with return code: {self.returncode()}")
            print(f"Process stdout: {self.stdout_accumulator}")
            print(f"Process stderr: {self.stderr_accumulator}")

        # Save the raw data to disk so we can use it again later if necessary.
        # For example we might want to change the size of each tick,
        # or we might find a bug in our processing code.
        caller_name = get_calling_function_name()
        time_label = datetime.now(tz=timezone.utc).strftime("%Y-%m-%d--%H-%M-%S.%f")
        with open(f"{time_label}_{caller_name}_{self.label}.out.txt", "w") as outfile:
            outfile.write(self.stdout_accumulator)
        with open(f"{time_label}_{caller_name}_{self.label}.err.txt", "w") as outfile:
            outfile.write(self.stderr_accumulator)

        transmit_regex = re.compile(
            r"^(\d+\.\d+) (sendto|send|recvfrom|recv)\(.*\) = (\d+)$"
        )
        send_accumulator = 0
        recv_accumulator = 0
        first_tick: Optional[int] = None
        previous_tick: Optional[int] = None
        if initial_timestamp is not None:
            first_tick = time2tick(initial_timestamp)
            previous_tick = first_tick
        send_bursts = []
        recv_bursts = []
        assert self.process_handle.poll() is not None
        for line_index, line in enumerate(self.stderr_accumulator.splitlines()):
            match = re.match(transmit_regex, line)
            if match is not None:
                timestamp = match.group(1)
                syscall = match.group(2)
                num_bytes = int(match.group(3))

                is_send = syscall.startswith("send")
                if first_tick is None:
                    first_tick = time2tick(float(timestamp))
                    previous_tick = time2tick(first_tick)
                assert previous_tick is not None

                new_tick = time2tick(float(timestamp))
                if new_tick != previous_tick:
                    prev_time = tick2time(previous_tick - first_tick)
                    send_bursts.append((prev_time, burst2mbps(send_accumulator)))
                    recv_bursts.append((prev_time, burst2mbps(recv_accumulator)))
                    send_accumulator = 0
                    recv_accumulator = 0

                    # Fill in zeroes for any intervening ticks that had no transmissions
                    for tick in range(previous_tick + 1, new_tick):
                        ticktime = tick2time(tick - first_tick)
                        send_bursts.append((ticktime, 0))
                        recv_bursts.append((ticktime, 0))
                    previous_tick = new_tick

                if is_send:
                    send_accumulator += num_bytes
                else:
                    recv_accumulator += num_bytes

        if (previous_tick is not None) and (first_tick is not None):
            last_time = tick2time(previous_tick - first_tick)
        else:
            last_time = 0
        if send_accumulator != 0:
            send_bursts.append((last_time, burst2mbps(send_accumulator)))
        if recv_accumulator != 0:
            recv_bursts.append((last_time, burst2mbps(recv_accumulator)))
        return (send_bursts, recv_bursts)


def run_for_seconds(
    seconds: float, procs_to_update: "Iterable[BandwidthTracingProcess]"
) -> bool:
    start = datetime.now()
    limit = timedelta(seconds=seconds)
    while (datetime.now() - start) < limit:
        for proc in procs_to_update:
            proc.accumulate_output()
        time.sleep(0.01)
    return any((p for p in procs_to_update if p.has_terminated()))


def run_to_completion(
    *procs: BandwidthTracingProcess,
    also_update_procs: "Iterable[BandwidthTracingProcess] | None" = None,
) -> None:
    all_terminated = False
    while not all_terminated:
        all_terminated = True
        for proc in procs:
            proc.accumulate_output()
            if not proc.has_terminated():
                all_terminated = False
        if also_update_procs is not None:
            for proc in also_update_procs:
                proc.accumulate_output()
        time.sleep(0.01)


def get_calling_function_name() -> str:
    current_stack_frame = inspect.currentframe()
    if current_stack_frame is None:
        return "<No stack info available>"

    # We need to go up 2 frames because to get the "calling function", we had to call this function
    requesting_stack_frame = current_stack_frame.f_back
    if requesting_stack_frame is None:
        return "<No requesting stack frame>"

    calling_stack_frame = requesting_stack_frame.f_back
    if calling_stack_frame is None:
        return "<No calling stack frame>"

    calling_function_name = calling_stack_frame.f_code.co_name
    return calling_function_name


def align_timeseries(series: "Sequence[TransferSeries]") -> None:
    if any(len(s) == 0 for s in series):
        return  # We have empty series, so don't try to align them

    min_start_tick = time2tick(min((s[0][0] for s in series)))
    max_end_tick = time2tick(max((s[-1][0] for s in series)))
    for s in series:
        start_tick = time2tick(s[0][0])
        end_tick = time2tick(s[-1][0])
        prefix_ticks = [(tick2time(t), 0) for t in range(min_start_tick, start_tick)]
        if len(prefix_ticks) > 0:
            s.reverse()
            prefix_ticks.reverse()
            s.extend(prefix_ticks)
            s.reverse()
        suffix_ticks = [
            (tick2time(t), 0) for t in range(end_tick + 1, max_end_tick + 1)
        ]
        s.extend(suffix_ticks)


def plot_series(
    series: TransferSeries,
    label: str,
    linestyle: str | None = None,
    print_stats: bool = True,
) -> None:
    (xvals, yvals) = zip(*series)
    if print_stats:
        print(f"{label} stats:")
        print(f"    Min = {min(yvals)}")
        print(f"    Max = {max(yvals)}")
        print(f"    Mean = {statistics.mean(yvals)}")
        print(f"    Stddev = {statistics.pstdev(yvals)}")

    plt.plot(xvals, yvals, label=label, linestyle=linestyle)


def plot_series_sum(
    series: "Sequence[TransferSeries]",
    label: str,
) -> None:
    if len(series) == 0:
        return

    summed: TransferSeries = []
    for series_index, s in enumerate(series):
        if len(s) != len(series[0]):
            print(
                f"ERROR: Cannot sum series with different lengths!: {len(s)} vs {len(series[0])}"
            )
            print(f"Series 0: {str(series[0])}")
            print(f"Series {series_index}: {str(s)}")
            raise ValueError("Cannot sum series with different lengths")

    for i in range(len(series[0])):
        for check_series_index, check_series in enumerate(series):
            if check_series[i][0] != series[0][i][0]:
                print(
                    f"ERROR: Cannot sum series {check_series_index} with mismatched timestamps @ "
                    + f"index {i}: {check_series[i][0]} vs {series[0][i][0]}"
                )
                print(f"Series 0: {str(series[0])}")
                print(f"Series {series_index}: {str(check_series)}")
                raise ValueError("Cannot sum series with mismatched timestamps")
        sum_val = sum((s[i][1] for s in series))
        summed.append((series[0][i][0], sum_val))
    plot_series(summed, label, linestyle="--")


def save_plot(show_legend: bool = True) -> None:
    title = get_calling_function_name()
    time_label = datetime.now(tz=timezone.utc).strftime("%Y-%m-%d--%H-%M-%S.%f")

    plt.title(title)
    plt.xlabel("Time (seconds)")
    plt.ylabel("Megabytes per second")
    plt.ylim(0, None)
    plt.xlim(0, None)
    if show_legend:
        plt.legend()
    plt.savefig(f"{time_label}_{title}.png", dpi=300.0)
    plt.clf()


def test_single_download_request() -> None:
    """
    Expect to see the download bandwidth restricted to the configured limit.
    """
    proc = BandwidthTracingProcess(get_cmd("100mb", server=1, user=1))
    run_to_completion(proc)
    (_, recv_rate) = proc.parse_network_stats()

    plot_series(recv_rate, label="Download")
    save_plot()
    assert proc.returncode() == 0


def test_single_upload_request() -> None:
    """
    Expect to see the download bandwidth restricted to the configured limit.
    """
    proc = BandwidthTracingProcess(post_cmd("50mb", server=1, user=1))
    run_to_completion(proc)
    (send_rate, recv_rate) = proc.parse_network_stats()

    plot_series(send_rate, label="Upload")
    save_plot()
    assert proc.returncode() == 0


def test_single_download_with_concurrent_single_upload() -> None:
    proc_recv = BandwidthTracingProcess(get_cmd("100mb", server=1, user=1))
    proc_send = BandwidthTracingProcess(post_cmd("50mb", server=1, user=1))
    run_to_completion(proc_recv, proc_send)
    (_, recv_rate) = proc_recv.parse_network_stats()
    (send_rate, _) = proc_send.parse_network_stats()

    plot_series(recv_rate, label="Download")
    plot_series(send_rate, label="Upload")
    save_plot()
    assert proc_recv.returncode() == 0
    assert proc_send.returncode() == 0


def test_two_concurrent_downloads_from_one_user_to_one_server() -> None:
    """
    Expect each download to be restricted to roughly half of the configured limit.
    """
    start_timestamp = datetime.now().timestamp()
    proc1 = BandwidthTracingProcess(get_cmd("100mb", server=1, user=1), "down1")
    proc2 = BandwidthTracingProcess(get_cmd("100mb", server=1, user=1), "down2")
    run_to_completion(proc1, proc2)
    (_, proc1_recv) = proc1.parse_network_stats(start_timestamp)
    (_, proc2_recv) = proc2.parse_network_stats(start_timestamp)
    align_timeseries((proc1_recv, proc2_recv))

    plot_series(proc1_recv, label="1st download")
    plot_series(proc2_recv, label="2nd download")
    plot_series_sum((proc1_recv, proc2_recv), label="Total download")
    save_plot()
    assert proc1.returncode() == 0
    assert proc2.returncode() == 0


def test_two_concurrent_downloads_from_two_users_to_one_server() -> None:
    """
    Expect each download to receive the full configured throughput limit of that user.
    """
    proc1 = BandwidthTracingProcess(get_cmd("200mb", server=1, user=1), "down1")
    proc2 = BandwidthTracingProcess(get_cmd("200mb", server=1, user=2), "down2")
    run_to_completion(proc1, proc2)

    (_, proc1_recv) = proc1.parse_network_stats()
    (_, proc2_recv) = proc2.parse_network_stats()
    align_timeseries((proc1_recv, proc2_recv))

    plot_series(proc1_recv, label="1st download")
    plot_series(proc2_recv, label="2nd download")
    plot_series_sum((proc1_recv, proc2_recv), label="Total download")
    save_plot()
    assert proc1.returncode() == 0
    assert proc2.returncode() == 0


def test_two_concurrent_downloads_from_one_user_to_two_servers() -> None:
    """
    Expect each download to be restricted to roughly one third of the configured limit.
    """
    start_timestamp = datetime.now().timestamp()
    proc1 = BandwidthTracingProcess(get_cmd("100mb", server=1, user=1), "down1")
    proc2 = BandwidthTracingProcess(get_cmd("100mb", server=2, user=1), "down2")
    run_to_completion(proc1, proc2)
    (_, proc1_recv) = proc1.parse_network_stats(start_timestamp)
    (_, proc2_recv) = proc2.parse_network_stats(start_timestamp)
    align_timeseries((proc1_recv, proc2_recv))

    plot_series(proc1_recv, label="1st download")
    plot_series(proc2_recv, label="2nd download")
    plot_series_sum((proc1_recv, proc2_recv), label="Total download")
    save_plot()
    assert proc1.returncode() == 0
    assert proc2.returncode() == 0


def test_three_concurrent_downloads_from_one_user_to_two_servers() -> None:
    """
    Expect each download to be restricted to roughly one third of the configured limit.
    """
    start_timestamp = datetime.now().timestamp()
    proc1 = BandwidthTracingProcess(get_cmd("100mb", server=1, user=1), "down1")
    proc2 = BandwidthTracingProcess(get_cmd("100mb", server=2, user=1), "down2")
    proc3 = BandwidthTracingProcess(get_cmd("50mb", server=2, user=1), "down3")
    run_to_completion(proc1, proc2, proc3)
    (_, proc1_recv) = proc1.parse_network_stats(start_timestamp)
    (_, proc2_recv) = proc2.parse_network_stats(start_timestamp)
    (_, proc3_recv) = proc3.parse_network_stats(start_timestamp)
    align_timeseries((proc1_recv, proc2_recv, proc3_recv))

    plot_series(proc1_recv, label="1st download")
    plot_series(proc2_recv, label="2nd download")
    plot_series(proc3_recv, label="3rd download (smaller)")
    plot_series_sum((proc1_recv, proc2_recv, proc3_recv), label="Total download")
    save_plot()
    assert proc1.returncode() == 0
    assert proc2.returncode() == 0
    assert proc3.returncode() == 0


def test_large_request_with_concurrent_small_request_to_the_same_server() -> None:
    """
    Expect the large download to start at the full configured download limit,
    drop down to about half when the second download starts, and then return
    to the full throughput once the smaller download completes.
    """
    start_timestamp = datetime.now().timestamp()
    proc1 = BandwidthTracingProcess(get_cmd("100mb", server=1, user=1))
    terminated_early = run_for_seconds(10, (proc1,))
    proc2 = BandwidthTracingProcess(get_cmd("20mb", server=1, user=1))

    run_to_completion(proc2, also_update_procs=(proc1,))
    terminated_early = terminated_early and proc1.has_terminated()
    run_to_completion(proc1)

    (_, proc1_recv) = proc1.parse_network_stats(start_timestamp)
    (_, proc2_recv) = proc2.parse_network_stats(start_timestamp)
    align_timeseries((proc1_recv, proc2_recv))

    plot_series(proc1_recv, label="Big download")
    plot_series(proc2_recv, label="Small download")
    plot_series_sum((proc1_recv, proc2_recv), label="Total download")
    save_plot()
    assert not terminated_early
    assert proc1.returncode() == 0
    assert proc2.returncode() == 0


def test_large_request_with_concurrent_small_request_to_a_different_server() -> None:
    """
    Expect the large download to start at the full configured download limit,
    drop down to about half when the second download starts, and then return
    to the full throughput once the smaller download completes.
    """
    start_timestamp = datetime.now().timestamp()
    proc1 = BandwidthTracingProcess(get_cmd("200mb", server=1, user=1))
    run_for_seconds(10, (proc1,))
    terminated_early = proc1.has_terminated()
    proc2 = BandwidthTracingProcess(get_cmd("50mb", server=2, user=1))

    run_to_completion(proc2, also_update_procs=(proc1,))
    terminated_early = terminated_early and proc1.has_terminated()
    run_to_completion(proc1)

    (_, proc1_recv) = proc1.parse_network_stats(start_timestamp)
    (_, proc2_recv) = proc2.parse_network_stats(start_timestamp)
    align_timeseries((proc1_recv, proc2_recv))

    plot_series(proc1_recv, label="Big download")
    plot_series(proc2_recv, label="Small download")
    plot_series_sum((proc1_recv, proc2_recv), label="Total download")
    save_plot()
    assert not terminated_early
    assert proc1.returncode() == 0
    assert proc2.returncode() == 0


# NOTE: This test should be run against a polygen instance where the user's GET request limit is at least 50/s
def test_many_small_requests_in_series_to_the_same_server() -> None:
    num_requests = 40
    cmd = get_cmd("512kb", server=1, user=1)
    for _ in range(num_requests):
        follow_on = get_cmd("512kb", server=1, user=1)
        # Replace 'curl' with '--next' to string all the requests together into one curl invocation
        follow_on[0] = "--next"
        cmd += follow_on

    proc = BandwidthTracingProcess(cmd)
    run_to_completion(proc)
    (_, recv_rate) = proc.parse_network_stats()

    plot_series(recv_rate, label="Download")
    save_plot()
    assert proc.returncode() == 0


# NOTE: This test should be run against a polygen instance where the user's GET request limit is at least 50/s
def test_many_small_requests_in_parallel_to_the_same_server() -> None:
    start_timestamp = datetime.now().timestamp()
    num_requests = 40
    requests: list[BandwidthTracingProcess] = []
    for i in range(num_requests):
        requests.append(BandwidthTracingProcess(get_cmd("512kb", server=1, user=1)))
        run_for_seconds(0.01, requests)

    run_to_completion(*requests)
    recv_results: list[int] = [proc.returncode() for proc in requests]
    recv_stats: list[TransferSeries] = [
        proc.parse_network_stats(start_timestamp)[1] for proc in requests
    ]
    align_timeseries(recv_stats)

    for i, small_recv in enumerate(recv_stats):
        plot_series(small_recv, label=f"Download {i}", print_stats=False)
    plot_series_sum(recv_stats, label="Total download")
    save_plot(show_legend=False)
    assert all((return_code == 0 for return_code in recv_results))


# NOTE: This test should be run against a polygen instance where the user's GET request limit is at least 20/s
def test_large_request_with_bursts_of_concurrent_small_requests() -> None:
    start_timestamp = datetime.now().timestamp()
    proc1 = BandwidthTracingProcess(get_cmd("80mb", server=1, user=1))
    run_for_seconds(10, (proc1,))

    all_small_recv_stats: list[TransferSeries] = []
    all_small_recv_results: list[int] = []
    burst_repeats = 5
    small_requests_per_burst = 10
    for i in range(burst_repeats):
        small_requests = [
            BandwidthTracingProcess(get_cmd("256kb", server=1, user=1))
            for i in range(small_requests_per_burst)
        ]
        run_to_completion(*small_requests, also_update_procs=(proc1,))
        all_small_recv_results += (proc.returncode() for proc in small_requests)
        all_small_recv_stats += [
            proc.parse_network_stats(start_timestamp)[1] for proc in small_requests
        ]
        run_for_seconds(5, (proc1,))

    terminated_early = proc1.has_terminated()
    run_to_completion(proc1)
    (_, proc1_recv) = proc1.parse_network_stats(start_timestamp)
    align_timeseries((proc1_recv, *all_small_recv_stats))

    plot_series(proc1_recv, label="Big download")
    for i, small_recv in enumerate(all_small_recv_stats):
        plot_series(small_recv, label=f"Small download {i}", print_stats=False)
    plot_series_sum((proc1_recv, *all_small_recv_stats), label="Total download")
    save_plot(show_legend=False)
    assert not terminated_early
    assert proc1.returncode() == 0
    assert all((return_code == 0 for return_code in all_small_recv_results))
