//! Hardcoded Claude API pricing (USD per million tokens) for the most common models.
//! Used by the tailer to fold transcript usage into a running cost estimate.
//!
//! Numbers below match Anthropic's published list pricing as of late 2025/early 2026.
//! Cache reads are billed at a discount; cache creation at a small premium.
//! If a model isn't in the table we fall back to a Sonnet-ish rate so the
//! display still shows something reasonable.

struct Price {
    input_per_mtok: f64,
    output_per_mtok: f64,
    cache_creation_per_mtok: f64,
    cache_read_per_mtok: f64,
}

fn lookup(model: &str) -> Price {
    let m = model.to_ascii_lowercase();
    if m.contains("opus") {
        Price { input_per_mtok: 15.0, output_per_mtok: 75.0, cache_creation_per_mtok: 18.75, cache_read_per_mtok: 1.50 }
    } else if m.contains("haiku") {
        Price { input_per_mtok: 1.0, output_per_mtok: 5.0, cache_creation_per_mtok: 1.25, cache_read_per_mtok: 0.10 }
    } else {
        // sonnet (default)
        Price { input_per_mtok: 3.0, output_per_mtok: 15.0, cache_creation_per_mtok: 3.75, cache_read_per_mtok: 0.30 }
    }
}

pub fn cost_usd_for(
    model: &str,
    input_tokens: u64,
    output_tokens: u64,
    cache_creation: u64,
    cache_read: u64,
) -> f64 {
    let p = lookup(model);
    (input_tokens as f64) / 1_000_000.0 * p.input_per_mtok
        + (output_tokens as f64) / 1_000_000.0 * p.output_per_mtok
        + (cache_creation as f64) / 1_000_000.0 * p.cache_creation_per_mtok
        + (cache_read as f64) / 1_000_000.0 * p.cache_read_per_mtok
}
