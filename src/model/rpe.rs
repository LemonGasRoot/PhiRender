use serde_json::{Map, Value};

pub type Object = Map<String, Value>;

pub fn object(fields: impl IntoIterator<Item = (impl Into<String>, Value)>) -> Value {
    Value::Object(fields.into_iter().map(|(k, v)| (k.into(), v)).collect())
}

pub fn beat(value: f64, denominator: i64) -> [i64; 3] {
    let total = (value * denominator as f64).round() as i64;
    let whole = total.div_euclid(denominator);
    let numerator = total.rem_euclid(denominator);
    let gcd = gcd(numerator, denominator);
    [whole, numerator / gcd, denominator / gcd]
}

fn gcd(mut a: i64, mut b: i64) -> i64 {
    while b != 0 {
        (a, b) = (b, a % b);
    }
    if a == 0 {
        1
    } else {
        a.abs()
    }
}

pub fn rpe_event(start: f64, end: f64, start_time: [i64; 3], end_time: [i64; 3]) -> Value {
    object([
        ("startTime", serde_json::json!(start_time)),
        ("endTime", serde_json::json!(end_time)),
        ("start", serde_json::json!(start)),
        ("end", serde_json::json!(end)),
        ("easingType", serde_json::json!(1)),
        ("easingLeft", serde_json::json!(0)),
        ("easingRight", serde_json::json!(1)),
        ("bezier", serde_json::json!(0)),
        ("bezierPoints", serde_json::json!([0, 0, 1, 1])),
        ("linkgroup", serde_json::json!(0)),
    ])
}
