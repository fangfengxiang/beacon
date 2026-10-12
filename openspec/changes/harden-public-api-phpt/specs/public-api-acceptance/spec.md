## ADDED Requirements

### Requirement: Committed snapshot isolation
Public APIs SHALL expose only committed nodes, retain unchanged services across incremental commits and treat committed empty lists as authoritative.

#### Scenario: Stage update then clear
- **GIVEN** services A and B are committed
- **WHEN** A is staged, committed, then cleared and committed
- **THEN** reads before commit show old A, reads after clear show no A, B remains unchanged, and an empty commit does not resurrect A

### Requirement: Safe node conversion
Governance SHALL reject empty, oversized or NUL-containing service names and skip nodes with invalid required fields, status or weight without contaminating subsequent entries.

#### Scenario: Invalid entry followed by valid entry
- **GIVEN** a node with invalid port followed by a valid node lacking optional methods
- **WHEN** storeNodes and commit run
- **THEN** only the valid node appears and no fields leak from the skipped node

### Requirement: Routing boundaries
Weighted selection SHALL exclude zero-weight nodes and return null for all-zero candidates. setOpt SHALL reject invalid strategy enums.

#### Scenario: Single positive weight
- **GIVEN** zero-weight and positive-weight healthy nodes
- **WHEN** weighted pick runs repeatedly
- **THEN** only the positive-weight node is selected

### Requirement: Request callback lifetime
Callbacks SHALL release captured objects at request shutdown and remain alive during invocation even if they clear their own slot.

#### Scenario: Self clearing callback
- **GIVEN** a registered callback with captured state
- **WHEN** it clears its registration while executing
- **THEN** invocation completes safely, subsequent invocation reports absent callback, and captured state is released

### Requirement: Public API acceptance
PHPT SHALL assert disabled mode, method metadata, health structure, filtering, exclusions and parameter errors through public classes using exact expected output.

#### Scenario: Disabled extension
- **GIVEN** beacon.enabled=0
- **WHEN** public methods are invoked
- **THEN** status reports degraded mode and shm-dependent operations return documented empty, null or false values with warnings
