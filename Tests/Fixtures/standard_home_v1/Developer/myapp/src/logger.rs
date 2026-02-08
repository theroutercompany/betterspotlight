use std::time::{SystemTime, UNIX_EPOCH};

pub fn info(message: &str) {
    println!("[INFO] {} {}", timestamp(), message);
}

pub fn warn(message: &str) {
    println!("[WARN] {} {}", timestamp(), message);
}

pub fn error(message: &str) {
    eprintln!("[ERROR] {} {}", timestamp(), message);
}

fn timestamp() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("time went backwards")
        .as_secs()
}
