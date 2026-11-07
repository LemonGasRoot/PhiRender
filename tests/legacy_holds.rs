#![cfg(feature = "render")]
#[test]
fn legacy_engine_official_hold_body_matches_phivideo_at_1080p() {
    let start = 66.38613861386138;
    let duration = 15.0 / 202.0;
    let end = start + duration;
    let pixels = |now| prpr::core::official_hold_body(start, end, now, 5.) * 1080. / 2.;
    assert!((pixels(start) - 240.59405940594).abs() < 1e-6);
    assert!((pixels(start + duration / 2.) - 120.29702970297).abs() < 1e-6);
    assert_eq!(pixels(end + 1.), 0.);
    assert!((pixels(start - 1.) - pixels(start)).abs() < 1e-6);
}
