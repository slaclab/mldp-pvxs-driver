#!/usr/bin/env python3
"""Unit checks for scripts/generate_mldp_sample_data.py without gRPC services."""

from __future__ import annotations

import importlib.util
import io
import sys
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from types import SimpleNamespace


SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "generate_mldp_sample_data.py"
SPEC = importlib.util.spec_from_file_location("generate_mldp_sample_data", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
generator = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = generator
SPEC.loader.exec_module(generator)


class SampleDataGeneratorTest(unittest.TestCase):
    def test_default_namespace_and_validation(self) -> None:
        self.assertEqual(generator.parse_args([]).namespace, "mldp_sample")
        self.assertEqual(generator.validate_namespace("demo_1:rf-a"), "demo_1:rf-a")
        with redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                generator.parse_args(["--namespace", "bad namespace"])

    def test_names_are_coherent_and_complete(self) -> None:
        namespace = "demo"
        pvs = generator.pv_names(namespace)
        self.assertEqual(len(pvs), generator.PV_COUNT)
        self.assertEqual(len(set(pvs)), generator.PV_COUNT)
        self.assertTrue(all(name.startswith("demo:") for name in pvs))
        configurations = generator.configuration_names(namespace)
        activations = generator.activation_ids(namespace)
        self.assertEqual(len(configurations), 4)
        self.assertEqual(activations, [f"{name}_activation" for name in configurations])

    def test_signal_is_deterministic_and_has_requested_length(self) -> None:
        values = generator.sample_values(3, count=12)
        self.assertEqual(values, generator.sample_values(3, count=12))
        self.assertEqual(len(values), 12)
        self.assertNotEqual(values[0], values[1])

    def test_bucket_samples_cover_the_full_signal_without_overlap(self) -> None:
        self.assertEqual(generator.BUCKET_COUNT * generator.SAMPLES_PER_BUCKET,
                         generator.SAMPLE_COUNT)
        bucket_values = [
            generator.sample_values(3, generator.SAMPLES_PER_BUCKET,
                                    bucket * generator.SAMPLES_PER_BUCKET)
            for bucket in range(generator.BUCKET_COUNT)
        ]
        self.assertEqual(
            [value for values in bucket_values for value in values],
            generator.sample_values(3),
        )

    def test_namespace_cleanup_deletes_activations_before_configurations_and_metadata(self) -> None:
        class Request:
            def __init__(self, **values: str) -> None:
                self.values = values

        class Response:
            def HasField(self, _: str) -> bool:
                return False

        calls: list[tuple[str, str]] = []

        class AnnotationStub:
            def deleteConfigurationActivation(self, request: Request, *, timeout: float) -> Response:
                self._record("activation", request.values["clientActivationId"], timeout)
                return Response()

            def deleteConfiguration(self, request: Request, *, timeout: float) -> Response:
                self._record("configuration", request.values["configurationName"], timeout)
                return Response()

            def deletePvMetadata(self, request: Request, *, timeout: float) -> Response:
                self._record("metadata", request.values["pvNameOrAlias"], timeout)
                return Response()

            @staticmethod
            def _record(kind: str, name: str, timeout: float) -> None:
                calls.append((kind, name))
                assert timeout == 7.0

        bindings = SimpleNamespace(annotation=SimpleNamespace(
            DeleteConfigurationActivationRequest=Request,
            DeleteConfigurationRequest=Request,
            DeletePvMetadataRequest=Request,
        ))
        generator.drop_namespace(bindings, AnnotationStub(), "demo", 7.0)

        self.assertEqual([kind for kind, _ in calls[:4]], ["activation"] * 4)
        self.assertEqual([kind for kind, _ in calls[4:8]], ["configuration"] * 4)
        self.assertEqual([kind for kind, _ in calls[8:]], ["metadata"] * generator.PV_COUNT)


if __name__ == "__main__":
    unittest.main()
