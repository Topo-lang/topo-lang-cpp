# Topo Quickstart: Order Processing (C++)

## What This Example Does

An order processing system with clear API boundaries and execution stages:

- **public**: `process_order()` -- the single entry point
- **protected**: `validate_order()`, `charge_payment()`, `calculate_shipping()`, `create_invoice()` -- reusable processing components
- **private**: `send_confirmation()`, `update_analytics()`, `check_inventory()`, `verify_address()`, `apply_discount()` -- implementation details
- **internal**: `dump_order_state()` -- debug/test helper

### What the Declarations Mean

The `.topo` file declares a 4-stage processing pipeline:

```
                        +-------------------+
          Stage 1:      | validate_order()  |
                        +---------+---------+
                                  |
                    +-------------+-------------+
                    |                           |
          Stage 2:  | charge_payment()    calculate_shipping() |
                    |  (parallel-safe)      (parallel-safe)    |
                    +-------------+-------------+
                                  |
          Stage 3:      +---------+---------+
                        | create_invoice()  |
                        +---------+---------+
                                  |
                    +-------------+-------------+
                    |                           |
          Stage 4:  | send_confirmation() update_analytics()   |
                    |  (parallel-safe)      (parallel-safe)    |
                    +-------------+-------------+
```

- **Stage 1**: `validate_order` -- must complete before anything else
- **Stage 2**: `charge_payment` + `calculate_shipping` -- independent operations that can safely run in parallel
- **Stage 3**: `create_invoice` -- depends on both stage-2 results
- **Stage 4**: `send_confirmation` + `update_analytics` -- independent post-processing

Topo enforces these constraints:
- Stage 2 cannot start before stage 1 completes
- `charge_payment` and `calculate_shipping` must not share mutable state (they are declared parallel-safe)
- Code outside the `orders` namespace cannot call `private` functions like `send_confirmation`

## Try It

### Step 1: Validate declarations

```sh
topo --check topo-lang-cpp/examples/quickstart/topo/processor.topo
```

### Step 2: Verify completeness

```sh
topo-test --project topo-lang-cpp/examples/quickstart --check-completeness
```

Expected output:

```
[PASS] Completeness: 11 host symbol(s) checked, 11 .topo function(s) -- all OK
```

### Step 3: Add an undeclared function --> ERROR

Add a new function to `src/processor.cpp` without updating the `.topo` file:

```cpp
namespace orders {
bool cancel_order(int order_id) { return true; }
}
```

Run the check again:

```sh
topo-test --project topo-lang-cpp/examples/quickstart --check-completeness
```

```
ERROR: symbol 'orders::cancel_order' exists in host code
       but is not declared in .topo
```

**Why it matters**: Someone added a new function but forgot to update the contract.
Topo catches this before compilation.

### Step 4: Remove a declared function --> WARNING

Delete the `calculate_shipping()` function body from `src/processor.cpp`
(keep the `.topo` declaration).

```sh
topo-test --project topo-lang-cpp/examples/quickstart --check-completeness
```

```
WARNING: function 'orders::calculate_shipping' is declared in .topo
         but not found in host code
```

### Step 5: Visibility mismatch

> **Note**: C++ free functions in namespaces have no access modifiers,
> so visibility mismatch checks do not apply to this C++ example.
> See the Rust or Java quickstart for visibility mismatch demonstrations.

### Step 6: Restore and pass

Undo all changes. Run the check again:

```sh
topo-test --project topo-lang-cpp/examples/quickstart --check-completeness
```

```
[PASS] Completeness: all OK
```

### Step 7: Verify stage isolation

Stage isolation verifies that stage N does not depend on stage N+1 outputs.
This requires a test command that exercises the pipeline:

```sh
topo-test --project topo-lang-cpp/examples/quickstart \
          --check-isolation \
          --test-cmd "echo 'pipeline-test-placeholder'"
```

Expected: each stage passes independently — stage 2 functions (`charge_payment`,
`calculate_shipping`) do not call stage 3 or 4 functions.

### Step 8: Verify parallel safety

Purity check verifies that functions in the same stage (declared parallel-safe)
do not access shared mutable state:

```sh
topo-test --project topo-lang-cpp/examples/quickstart \
          --check-purity \
          --test-cmd "echo 'pipeline-test-placeholder'"
```

Expected: stage 2 (`charge_payment` + `calculate_shipping`) and stage 4
(`send_confirmation` + `update_analytics`) pass — no shared global state.

## What You Learned

| Violation | Severity | Topo Report |
|-----------|----------|-------------|
| Code has function, `.topo` does not declare it | ERROR | "exists in host code but not declared" |
| `.topo` declares function, code does not implement it | WARNING | "declared but not found in host code" |
| Stage N calls stage N+1 function | ERROR | "stage isolation violated" |
| Parallel-stage function writes global state | ERROR | "purity violation" |

For visibility mismatch checks, see the Rust or Java quickstart.

## What's Next

- **Showcase example**: demonstrates all Topo features -- constraints, templates, pipeline DAG, lifetime management, priority, ownership
