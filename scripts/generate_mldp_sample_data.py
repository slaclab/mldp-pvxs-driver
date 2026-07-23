#!/usr/bin/env python3
# Copyright (c) 2026 SLAC National Accelerator Laboratory
# Distributed subject to the EPICS Open License found in the LICENSE.txt file.
"""Populate an MLDP stack with coherent time-series and annotation demo data.

The script uses the pinned dp_grpc proto sources fetched by CMake.  It generates
Python protobuf/gRPC bindings in a disposable user cache when they are absent;
no generated files are added to the repository.

Examples (run inside the devcontainer):

  python3 scripts/generate_mldp_sample_data.py --verify
  python3 scripts/generate_mldp_sample_data.py --namespace accelerator_demo
  python3 scripts/generate_mldp_sample_data.py --drop-namespace

Dependencies: grpcio, grpcio-tools, protobuf.
Install them in an isolated environment, for example:

  python3 -m pip install grpcio grpcio-tools protobuf

``--drop-namespace`` deletes only this script's PV metadata, configurations,
and configuration activations.  The MLDP gRPC artifacts do not expose an API
to delete archived time-series buckets, so already-ingested samples remain.
"""

from __future__ import annotations

import argparse
import importlib
import math
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from types import ModuleType
from typing import Sequence


DEFAULT_NAMESPACE = "mldp_sample"
PV_COUNT = 20
SAMPLE_COUNT = 3600
SAMPLE_PERIOD_NANOS = 1_000_000_000
DEVICE_FAMILIES = ("MAGNET", "RF", "VACUUM", "DIAGNOSTIC")


class SampleDataError(RuntimeError):
    """Raised when MLDP rejects a request or the local setup is incomplete."""


@dataclass(frozen=True)
class Bindings:
    common: ModuleType
    ingestion: ModuleType
    ingestion_grpc: ModuleType
    annotation: ModuleType
    annotation_grpc: ModuleType


def repository_root() -> Path:
    return Path(__file__).resolve().parents[1]


def validate_namespace(value: str) -> str:
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_:-]*", value):
        raise argparse.ArgumentTypeError(
            "namespace must start with a letter and contain only letters, digits, '_', ':', or '-'"
        )
    return value


def proto_source_dir() -> Path:
    candidates = (
        repository_root() / "build" / "_deps" / "dp_grpc-src" / "src" / "main" / "proto",
        repository_root() / "_deps" / "dp_grpc-src" / "src" / "main" / "proto",
    )
    for candidate in candidates:
        if (candidate / "common.proto").is_file():
            return candidate
    raise SampleDataError(
        "Cannot find pinned dp_grpc proto sources. Configure the project first "
        "(for example, run the devcontainer CMake configure step)."
    )


def generated_bindings_dir() -> Path:
    return Path.home() / ".cache" / "mldp-pvxs-driver" / "python-grpc" / "dp_grpc"


def load_bindings() -> Bindings:
    try:
        import grpc  # noqa: F401
        import grpc_tools.protoc
    except ImportError as error:
        raise SampleDataError(
            "Missing Python gRPC dependencies. Install with: "
            "python3 -m pip install grpcio grpcio-tools protobuf"
        ) from error

    output_dir = generated_bindings_dir()
    required = ("common_pb2.py", "ingestion_pb2.py", "ingestion_pb2_grpc.py", "annotation_pb2.py", "annotation_pb2_grpc.py")
    if not all((output_dir / item).is_file() for item in required):
        output_dir.mkdir(parents=True, exist_ok=True)
        proto_dir = proto_source_dir()
        command = [
            sys.executable,
            "-m",
            "grpc_tools.protoc",
            f"--proto_path={proto_dir}",
            f"--python_out={output_dir}",
            f"--grpc_python_out={output_dir}",
            str(proto_dir / "common.proto"),
            str(proto_dir / "ingestion.proto"),
            str(proto_dir / "annotation.proto"),
        ]
        completed = subprocess.run(command, text=True, capture_output=True, check=False)
        if completed.returncode != 0:
            raise SampleDataError(f"Failed to generate Python gRPC artifacts:\n{completed.stderr.strip()}")

    output = str(output_dir)
    if output not in sys.path:
        sys.path.insert(0, output)
    for name in ("common_pb2", "ingestion_pb2", "ingestion_pb2_grpc", "annotation_pb2", "annotation_pb2_grpc"):
        sys.modules.pop(name, None)
    return Bindings(
        common=importlib.import_module("common_pb2"),
        ingestion=importlib.import_module("ingestion_pb2"),
        ingestion_grpc=importlib.import_module("ingestion_pb2_grpc"),
        annotation=importlib.import_module("annotation_pb2"),
        annotation_grpc=importlib.import_module("annotation_pb2_grpc"),
    )


def response_error(response: object) -> str | None:
    if hasattr(response, "HasField") and response.HasField("exceptionalResult"):
        return response.exceptionalResult.message or "server returned an exceptional result"
    return None


def require_success(rpc_name: str, response: object) -> None:
    if error := response_error(response):
        raise SampleDataError(f"{rpc_name} rejected by MLDP: {error}")


def call_rpc(rpc_name: str, method: object, request: object, timeout: float) -> object:
    try:
        return method(request, timeout=timeout)
    except Exception as error:
        raise SampleDataError(f"{rpc_name} transport failure: {error}") from error


def pv_names(namespace: str) -> list[str]:
    names: list[str] = []
    for index in range(PV_COUNT):
        family = DEVICE_FAMILIES[index % len(DEVICE_FAMILIES)]
        names.append(f"{namespace}:{family}:{index + 1:02d}:VALUE")
    return names


def configuration_names(namespace: str) -> list[str]:
    return [
        f"{namespace}_injector_tuning",
        f"{namespace}_user_delivery",
        f"{namespace}_rf_station_a",
        f"{namespace}_vacuum_ready",
    ]


def activation_ids(namespace: str) -> list[str]:
    return [f"{name}_activation" for name in configuration_names(namespace)]


def sample_values(index: int, count: int = SAMPLE_COUNT) -> list[float]:
    """Return deterministic smooth values with a family-specific offset."""
    baseline = 10.0 * (index + 1)
    amplitude = 0.25 + (index % 5) * 0.15
    period = 60.0 + (index % 4) * 30.0
    return [baseline + amplitude * math.sin(2.0 * math.pi * sample / period) for sample in range(count)]


def add_attribute(target: object, name: str, value: str) -> None:
    attribute = target.add()
    attribute.name = name
    attribute.value = value


def make_ingest_request(bindings: Bindings, namespace: str, provider_id: str, start_time: int):
    request = bindings.ingestion.IngestDataRequest(
        providerId=provider_id,
        clientRequestId=f"{namespace}-samples-{start_time}",
    )
    clock = request.ingestionDataFrame.dataTimestamps.samplingClock
    clock.startTime.epochSeconds = start_time
    clock.startTime.nanoseconds = 0
    clock.periodNanos = SAMPLE_PERIOD_NANOS
    clock.count = SAMPLE_COUNT
    for index, pv_name in enumerate(pv_names(namespace)):
        column = request.ingestionDataFrame.doubleColumns.add()
        column.name = pv_name
        column.values.extend(sample_values(index))
        column.metadata.provenance.source = f"sample-generator/{namespace}"
        column.metadata.provenance.process = "deterministic-sine"
        column.metadata.tags.extend(("sample", namespace.lower(), DEVICE_FAMILIES[index % len(DEVICE_FAMILIES)].lower()))
        add_attribute(column.metadata.attributes, "namespace", namespace)
        add_attribute(column.metadata.attributes, "ordinal", str(index + 1))
        add_attribute(column.metadata.attributes, "sample_period_seconds", "1")
    return request


def save_metadata(bindings: Bindings, annotation_stub: object, namespace: str, timeout: float) -> None:
    for index, pv_name in enumerate(pv_names(namespace)):
        family = DEVICE_FAMILIES[index % len(DEVICE_FAMILIES)]
        request = bindings.annotation.SavePvMetadataRequest(
            pvName=pv_name,
            aliases=[f"{namespace}:ALIAS:{index + 1:02d}"],
            tags=["sample", namespace.lower(), family.lower()],
            modifiedBy="generate_mldp_sample_data.py",
            description=f"Deterministic {family.lower()} sample signal {index + 1}",
        )
        add_attribute(request.attributes, "namespace", namespace)
        add_attribute(request.attributes, "device_group", family)
        add_attribute(request.attributes, "ordinal", str(index + 1))
        add_attribute(request.attributes, "units", "arb")
        add_attribute(request.attributes, "sample_period_seconds", "1")
        require_success("savePvMetadata", call_rpc("savePvMetadata", annotation_stub.savePvMetadata, request, timeout))


def timestamp(target: object, seconds: int) -> None:
    target.epochSeconds = seconds
    target.nanoseconds = 0


def save_configurations(bindings: Bindings, annotation_stub: object, namespace: str, now: int, timeout: float) -> None:
    names = configuration_names(namespace)
    definitions = (
        (names[0], "beam_mode", "Injector tuning mode", None),
        (names[1], "beam_mode", "User delivery mode", names[0]),
        (names[2], "rf", "RF station A nominal settings", None),
        (names[3], "vacuum", "Vacuum system ready state", None),
    )
    for name, category, description, parent in definitions:
        request = bindings.annotation.SaveConfigurationRequest(
            configurationName=name,
            category=category,
            description=description,
            modifiedBy="generate_mldp_sample_data.py",
            tags=["sample", namespace.lower(), category],
        )
        if parent:
            request.parentConfigurationName = parent
        add_attribute(request.attributes, "namespace", namespace)
        require_success("saveConfiguration", call_rpc("saveConfiguration", annotation_stub.saveConfiguration, request, timeout))

    # The two beam-mode windows are adjacent and closed; the independent RF
    # and vacuum categories remain open, producing coherent active states.
    windows = ((names[0], now - 7200, now - 3600), (names[1], now - 3600, now), (names[2], now - 1800, None), (names[3], now - 900, None))
    for activation_id, (name, start, end) in zip(activation_ids(namespace), windows, strict=True):
        request = bindings.annotation.SaveConfigurationActivationRequest(
            clientActivationId=activation_id,
            configurationName=name,
            description=f"Sample activation for {name}",
            modifiedBy="generate_mldp_sample_data.py",
            tags=["sample", namespace.lower()],
        )
        timestamp(request.startTime, start)
        if end is not None:
            timestamp(request.endTime, end)
        add_attribute(request.attributes, "namespace", namespace)
        require_success("saveConfigurationActivation", call_rpc("saveConfigurationActivation", annotation_stub.saveConfigurationActivation, request, timeout))


def register_provider(bindings: Bindings, ingestion_stub: object, namespace: str, timeout: float) -> str:
    request = bindings.ingestion.RegisterProviderRequest(
        providerName=f"{namespace}-sample-provider",
        description="Deterministic MLDP sample dataset generated by generate_mldp_sample_data.py",
        tags=["sample", namespace.lower()],
    )
    add_attribute(request.attributes, "namespace", namespace)
    response = call_rpc("registerProvider", ingestion_stub.registerProvider, request, timeout)
    require_success("registerProvider", response)
    if not response.HasField("registrationResult") or not response.registrationResult.providerId:
        raise SampleDataError("registerProvider returned no provider ID")
    return response.registrationResult.providerId


def drop_namespace(bindings: Bindings, annotation_stub: object, namespace: str, timeout: float) -> None:
    for activation_id in activation_ids(namespace):
        request = bindings.annotation.DeleteConfigurationActivationRequest(clientActivationId=activation_id)
        require_success("deleteConfigurationActivation", call_rpc("deleteConfigurationActivation", annotation_stub.deleteConfigurationActivation, request, timeout))
    for name in configuration_names(namespace):
        request = bindings.annotation.DeleteConfigurationRequest(configurationName=name)
        require_success("deleteConfiguration", call_rpc("deleteConfiguration", annotation_stub.deleteConfiguration, request, timeout))
    for pv_name in pv_names(namespace):
        request = bindings.annotation.DeletePvMetadataRequest(pvNameOrAlias=pv_name)
        require_success("deletePvMetadata", call_rpc("deletePvMetadata", annotation_stub.deletePvMetadata, request, timeout))


def verify_namespace(bindings: Bindings, annotation_stub: object, namespace: str, timeout: float) -> None:
    for pv_name in pv_names(namespace):
        response = call_rpc("getPvMetadata", annotation_stub.getPvMetadata,
                            bindings.annotation.GetPvMetadataRequest(pvNameOrAlias=pv_name), timeout)
        require_success("getPvMetadata", response)
    for name in configuration_names(namespace):
        response = call_rpc("getConfiguration", annotation_stub.getConfiguration,
                            bindings.annotation.GetConfigurationRequest(configurationName=name), timeout)
        require_success("getConfiguration", response)


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--namespace", type=validate_namespace, default=DEFAULT_NAMESPACE, help=f"Dataset prefix (default: {DEFAULT_NAMESPACE})")
    parser.add_argument("--ingestion-url", default="dp-ingestion:50051", help="MLDP ingestion gRPC endpoint")
    parser.add_argument("--annotation-url", default="dp-annotation:50053", help="MLDP annotation gRPC endpoint")
    parser.add_argument("--timeout-seconds", type=float, default=15.0, help="Deadline for each RPC (default: 15)")
    parser.add_argument("--drop-namespace", action="store_true", help="Delete generated annotation records only; does not delete time-series buckets")
    parser.add_argument("--verify", action="store_true", help="Read back generated PV metadata and configuration records")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if args.timeout_seconds <= 0:
        raise SampleDataError("--timeout-seconds must be positive")
    bindings = load_bindings()
    import grpc

    annotation_channel = grpc.insecure_channel(args.annotation_url)
    annotation_stub = bindings.annotation_grpc.DpAnnotationServiceStub(annotation_channel)
    try:
        if args.drop_namespace:
            drop_namespace(bindings, annotation_stub, args.namespace, args.timeout_seconds)
            print(f"Deleted namespace-scoped annotation records for '{args.namespace}'.")
            print("Archived time-series samples remain: the MLDP gRPC API has no bucket deletion operation.")
            return 0

        ingestion_channel = grpc.insecure_channel(args.ingestion_url)
        ingestion_stub = bindings.ingestion_grpc.DpIngestionServiceStub(ingestion_channel)
        provider_id = register_provider(bindings, ingestion_stub, args.namespace, args.timeout_seconds)
        start_time = int(time.time()) - SAMPLE_COUNT
        request = make_ingest_request(bindings, args.namespace, provider_id, start_time)
        require_success("ingestData", call_rpc("ingestData", ingestion_stub.ingestData, request, args.timeout_seconds))
        save_metadata(bindings, annotation_stub, args.namespace, args.timeout_seconds)
        save_configurations(bindings, annotation_stub, args.namespace, int(time.time()), args.timeout_seconds)
        if args.verify:
            verify_namespace(bindings, annotation_stub, args.namespace, args.timeout_seconds)

        print(f"Generated MLDP sample namespace: {args.namespace}")
        print(f"Provider: {args.namespace}-sample-provider ({provider_id})")
        print(f"Time series: {PV_COUNT} PVs x {SAMPLE_COUNT} samples at 1-second intervals")
        print(f"Time range: [{start_time}, {start_time + SAMPLE_COUNT - 1}] UTC epoch seconds")
        print("Configurations: " + ", ".join(configuration_names(args.namespace)))
        print("Activation IDs: " + ", ".join(activation_ids(args.namespace)))
        print(f"Example query: SELECT pv, time, value FROM mldp.time_series WHERE pv = '{pv_names(args.namespace)[0]}'")
        return 0
    finally:
        annotation_channel.close()
        if "ingestion_channel" in locals():
            ingestion_channel.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except SampleDataError as error:
        print(f"Error: {error}", file=sys.stderr)
        raise SystemExit(1)
