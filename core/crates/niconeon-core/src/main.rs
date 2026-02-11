use std::io::{self, BufRead, Write};

use niconeon_core::AppCore;
use niconeon_fetcher::NiconicoFetcher;
use niconeon_protocol::{JsonRpcRequest, JsonRpcResponse};
use niconeon_store::Store;
use serde_json::json;

fn main() -> anyhow::Result<()> {
    let args: Vec<String> = std::env::args().collect();
    if args.iter().any(|a| a == "--help" || a == "-h") {
        println!("niconeon-core --stdio");
        return Ok(());
    }

    if !args.iter().any(|a| a == "--stdio") {
        eprintln!("only --stdio mode is supported");
        std::process::exit(2);
    }

    let store = Store::open_default()?;
    let cookie = std::env::var("NICONICO_COOKIE").ok();
    let fetcher = NiconicoFetcher::new(cookie)?;
    let mut app = AppCore::new(store, fetcher)?;

    let stdin = io::stdin();
    let mut stdout = io::stdout();

    for line in stdin.lock().lines() {
        let line = line?;
        if line.trim().is_empty() {
            continue;
        }

        let response = match serde_json::from_str::<JsonRpcRequest>(&line) {
            Ok(req) => app.handle_request(req),
            Err(e) => JsonRpcResponse::failure(json!(null), -32700, format!("parse error: {e}")),
        };

        let encoded = serde_json::to_string(&response)?;
        writeln!(stdout, "{encoded}")?;
        stdout.flush()?;
    }

    Ok(())
}
