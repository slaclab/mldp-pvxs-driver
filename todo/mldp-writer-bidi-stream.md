# Plan: Switch MLDPWriter to ingestDataBidiStream

## Problem

Current `ingestDataStream` (client-streaming) gives zero per-request feedback.
Driver sends all requests, calls `WritesDone()`, gets ONE summary response.
Server validates async — rejects show up only in MongoDB `requestStatus` collection.

Root cause of current ingestion failure: timestamps have `nanos=1772849062` (exceeds max 999999999).
Server rejects every request but driver never sees WHY — only "one or more requests were rejected".

## Target

Switch to `ingestDataBidiStream` — bidi-streaming with per-request ack/reject response.
Each request gets immediate validation feedback including rejection reason.

## Proto Reference (ingestion.proto:70-91)

```protobuf
rpc ingestDataBidiStream (stream IngestDataRequest) returns (stream IngestDataResponse);
```

`IngestDataResponse`:
- `providerId` + `clientRequestId` (echo back for correlation)
- `oneof result { ExceptionalResult | AckResult }`
- `ExceptionalResult` contains `.msg` with actual rejection reason

## Files to Modify

| File | Change |
|------|--------|
| `src/writer/mldp/MLDPWriter.cpp` | workerLoop, ensureStream, closeStream |
| `include/writer/mldp/MLDPWriter.h` | StreamState struct (line ~108) |

## Implementation Steps

### Step 1: Change stream type in StreamState

```cpp
// Before (line 50 of MLDPWriter.h):
std::unique_ptr<grpc::ClientWriter<IngestDataRequest>> writer;
dp::service::ingestion::IngestDataStreamResponse response;

// After:
std::unique_ptr<grpc::ClientReaderWriter<IngestDataRequest, IngestDataResponse>> stream;
```

### Step 2: Update ensureStream() (~line 491 of MLDPWriter.cpp)

```cpp
// Before:
state.writer = stub->ingestDataStream(&state.context, &state.response);

// After:
state.stream = stub->ingestDataBidiStream(&state.context);
```

### Step 3: Add per-message ack read after each Write()

In workerLoop, after successful `Write(req)`:

```cpp
IngestDataResponse response;
if (state.stream->Read(&response)) {
    if (response.has_exceptionalresult()) {
        errorf(*logger_, "Request {} rejected: {}",
               response.clientrequestid(),
               response.exceptionalresult().msg());
        // increment rejected counter
    } else {
        // increment accepted counter
        tracef(*logger_, "Request {} accepted", response.clientrequestid());
    }
}
```

### Step 4: Update closeStream()

```cpp
// Before:
state.writer->WritesDone();
auto status = state.writer->Finish();
// ... check state.response (IngestDataStreamResponse)

// After:
state.stream->WritesDone();
// Drain any remaining responses
IngestDataResponse response;
while (state.stream->Read(&response)) {
    if (response.has_exceptionalresult()) {
        errorf(*logger_, "Request {} rejected: {}",
               response.clientrequestid(),
               response.exceptionalresult().msg());
    }
}
auto status = state.stream->Finish();
if (!status.ok()) {
    errorf(*logger_, "Stream finished with error: {}", status.error_message());
}
```

### Step 5: Remove IngestDataStreamResponse from StreamState

No longer needed — responses come inline per-request.

### Step 6: Add metrics

- Counter: `ingestion_requests_accepted_total`
- Counter: `ingestion_requests_rejected_total`
- Log rejection reason at Error level (includes the actual validation message)

## Throughput Consideration

Synchronous `Read()` after each `Write()` adds ~1 RTT per request.
Mitigation: `max-conn: 4` spreads load across 4 streams.
Future optimization: batch N writes then read N responses (pipelining).

## Test Impact

- Integration tests use mock writer (no real gRPC) — unaffected
- Unit tests for MLDPWriter may need mock bidi stream
- Existing test suite should pass unchanged

## Verification

After implementation, re-run HDF5 ingestion:
1. Should see per-request "rejected: nanos=1772849062 invalid" in driver logs
2. Fix timestamp column swap (already done via dynamic block2_items detection)
3. Re-run — should see per-request "accepted" and data in MongoDB buckets collection
