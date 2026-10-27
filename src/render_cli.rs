#[path = "render_cli_shared.rs"]
mod shared;

use shared::{conf, RunMode};

#[macroquad::main(conf)]
async fn main() {
    shared::run(RunMode::Integrated).await;
}
