Tasks

Task 1: Characterize MongoDB Expansion Factor

Import the representative HDF5 file into MLDP and measure the resulting MongoDB storage requirements.

Please collect the following metrics:

HDF5 file size

MongoDB dataSize

MongoDB storageSize

MongoDB totalSize

MongoDB totalIndexSize

Number of MongoDB bucket documents

Average bucket document size

Average number of samples per bucket

Number of indexes created

Please compute:

MongoDB Expansion Factor

=

MongoDB Total Storage

/

Original HDF5 Size
This will become the primary metric used to forecast Run 26 storage requirements.

Do not estimate. Please measure.

Task 2: Characterize Ingestion Performance

Measure ingestion performance using the representative HDF5 file.

Please collect:

Total ingestion time

Average throughput (MB/sec)

CPU utilization

Memory utilization

Peak memory utilization

Peak CPU utilization

Please identify any bottlenecks.

Examples include:

CPU bottlenecks

Memory bottlenecks

MongoDB write bottlenecks

Network bottlenecks

Serialization bottlenecks

Task 3: Characterize Query Performance

Please benchmark several representative queries.

Examples:

Query A:

Retrieve 100 PVs over a 10-minute interval.

Query B:

Retrieve 1000 PVs at a single timestamp.

Query C:

Retrieve all telemetry channels for a 1-hour interval.

Please measure:

Query execution time

CPU utilization

Memory utilization

Please identify any obvious performance bottlenecks.

Task 4: Characterize Bucketing Strategy

Please document the current MLDP bucket implementation.

Specifically answer the following questions:

How are buckets constructed?

What is the bucket time interval?

How many samples are stored per bucket?

Is bucket size configurable?

What metadata is stored per bucket?

How are timestamps stored?

How are values stored?

What indexes are required?

Please provide recommendations if additional optimizations are possible.

Task 5: Run 26 Storage Forecast

Using the measured MongoDB expansion factor, please provide a Run 26 storage forecast.

Current HDF5 estimates are:

March 2026 : 980 GB

April 2026 : 525 GB

May 2026 : 999 GB

June 2026 : 469 GB

July 2026 : < 1 TB

Total Run 26 baseline ≈ 3 - 4 TB
Please estimate:

MongoDB storage requirements

Index storage requirements

Metadata storage requirements

Total MLDP storage requirements

Please provide:

Conservative estimate

Expected estimate

Worst-case estimate

Task 6: Future Direct Ingestion Architecture

HDF5 ingestion is only one use case.

The long-term architecture should also support direct accelerator telemetry ingestion.

Please evaluate and document:

Current architecture:

Accelerator Telemetry → HDF5 → MLDP

Future architecture:

Accelerator Telemetry → MLDP

Please discuss:

Advantages

Disadvantages

Operational impacts

Performance implications

Storage implications

Please identify any additional infrastructure requirements.

Examples:

Additional buffering

Streaming services

Intermediate queues

Temporary storage

Task 7: Long-Term Data Management Strategy

Please evaluate the following long-term storage layers.

Scientific Storage

HDF5 on SDF

Purpose:

Scientific source of truth

Immutable datasets

Long-term preservation

Operational Storage

MongoDB inside MLDP

Purpose:

Queryable operational datasets

Metadata

Provenance

Derived machine state

Machine Learning Storage

Purpose:

Feature engineered datasets

Training datasets

Validation datasets

Machine learning models

Please provide recommendations regarding:

Data ownership

Retention policies

Backup strategies

Long-term sustainability