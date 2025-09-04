use rand::{Rng, rng};
use regex::Regex;
use std::collections::HashSet;
use std::time::Duration;

const MAX_FILENAME_LENGTH: usize = 255;
const MAX_EXTENSION_LENGTH: usize = 10;
const DEFAULT_FILENAME_LENGTH: usize = 25;
const MIN_RANDOM_STRING_LENGTH: usize = 1;
const MAX_RANDOM_STRING_LENGTH: usize = 1000;

pub fn format_time(seconds: f64) -> String {
    if seconds < 0.0 {
        return "unknown".to_string();
    }

    let duration = Duration::from_secs_f64(seconds);
    let total_secs = duration.as_secs();
    let hours = total_secs / 3600;
    let mins = (total_secs % 3600) / 60;
    let secs = total_secs % 60;

    if hours > 0 {
        format!("{:02}:{:02}:{:02}", hours, mins, secs)
    } else {
        format!("{:02}:{:02}", mins, secs)
    }
}

fn validate_string(s: &str, max_length: usize) -> bool {
    !s.is_empty() && s.len() <= max_length
}

fn to_lower(s: &mut String) {
    s.make_ascii_lowercase();
}

pub fn get_extension(filename: &str) -> Option<String> {
    if !validate_string(filename, MAX_FILENAME_LENGTH) {
        log::info!("Invalid input filename");
        return None;
    }

    let ext_pattern = match Regex::new(r"(?i).*\.(opus|mp3|m4a|m4b)$") {
        Ok(pattern) => pattern,
        Err(e) => {
            log::info!("Regex compilation error: {}", e);
            return None;
        }
    };

    if let Some(captures) = ext_pattern.captures(filename) {
        if let Some(ext_match) = captures.get(1) {
            let mut ext = ext_match.as_str().to_string();
            to_lower(&mut ext);

            if ext.len() > MAX_EXTENSION_LENGTH {
                log::info!("Extension too long: {}", ext.len());
                return None;
            }
            return Some(ext);
        }
    }

    log::info!("No valid extension found in: {}", filename);
    None
}

pub fn generate_filename(original_filename: &str) -> Option<String> {
    if !validate_string(original_filename, MAX_FILENAME_LENGTH) {
        log::info!("Invalid input filename");
        return None;
    }

    let ext = get_extension(original_filename)?;
    let random_part = generate_random_string(DEFAULT_FILENAME_LENGTH);

    if random_part.is_empty() {
        log::info!("Failed to generate random filename");
        return None;
    }

    let result = format!("{}.{}", random_part, ext);

    if result.len() > MAX_FILENAME_LENGTH {
        log::info!("Generated filename too long");
        return None;
    }

    Some(result)
}

pub fn generate_unique_ints(count: i32, min_val: i32, max_val: i32) -> Option<Vec<i32>> {
    if count <= 0 {
        log::info!("Invalid count: {}", count);
        return None;
    }

    if min_val > max_val {
        log::info!("Invalid range: min={}, max={}", min_val, max_val);
        return None;
    }

    let range = (max_val as i64) - (min_val as i64) + 1;
    if (count as i64) > range {
        log::info!("Count ({}) exceeds range ({})", count, range);
        return None;
    }

    let mut result = Vec::with_capacity(count as usize);
    let mut generated = HashSet::new();
    let mut thread_rng = rng();

    while (result.len() as i32) < count {
        let value = thread_rng.random_range(min_val..=max_val);
        if generated.insert(value) {
            result.push(value);
        }
    }

    Some(result)
}

pub fn generate_random_string(length: usize) -> String {
    if length < MIN_RANDOM_STRING_LENGTH || length > MAX_RANDOM_STRING_LENGTH {
        log::info!("Invalid length: {}", length);
        return String::new();
    }

    const ALPHABET: &[u8] = b"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    let mut thread_rng = rng();
    let mut result = String::with_capacity(length);

    for _ in 0..length {
        let idx = thread_rng.random_range(0..ALPHABET.len());
        result.push(ALPHABET[idx] as char);
    }

    result
}

pub fn format_commas(num: i64) -> String {
    let str_num = num.to_string();
    let negative = str_num.starts_with('-');
    let start = if negative { 1 } else { 0 };
    let digits = &str_num[start..];
    let len = digits.len();

    if len <= 3 {
        return str_num;
    }

    let mut result = String::new();
    if negative {
        result.push('-');
    }

    let first_group_size = if len % 3 == 0 { 3 } else { len % 3 };
    result.push_str(&digits[..first_group_size]);

    let mut i = first_group_size;
    while i < len {
        result.push(',');
        let end = (i + 3).min(len);
        result.push_str(&digits[i..end]);
        i += 3;
    }

    result
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_format_time() {
        assert_eq!(format_time(0.0), "00:00");
        assert_eq!(format_time(30.0), "00:30");
        assert_eq!(format_time(90.0), "01:30");
        assert_eq!(format_time(3661.0), "01:01:01");
        assert_eq!(format_time(-1.0), "unknown");
    }

    #[test]
    fn test_get_extension() {
        assert_eq!(get_extension("song.mp3"), Some("mp3".to_string()));
        assert_eq!(get_extension("audiobook.M4B"), Some("m4b".to_string()));
        assert_eq!(get_extension("music.OPUS"), Some("opus".to_string()));
        assert_eq!(get_extension("audio.m4a"), Some("m4a".to_string()));
        assert_eq!(get_extension("invalid.txt"), None);
        assert_eq!(get_extension("noextension"), None);
        assert_eq!(get_extension(""), None);
    }

    #[test]
    fn test_generate_filename() {
        let result = generate_filename("test.mp3");
        assert!(result.is_some());
        let filename = result.unwrap();
        assert!(filename.ends_with(".mp3"));
        assert_eq!(filename.len(), DEFAULT_FILENAME_LENGTH + 4);
        assert_eq!(generate_filename("invalid.txt"), None);
        assert_eq!(generate_filename(""), None);
    }

    #[test]
    fn test_generate_unique_ints() {
        let result = generate_unique_ints(5, 1, 10);
        assert!(result.is_some());
        let ints = result.unwrap();
        assert_eq!(ints.len(), 5);

        let mut unique_check = HashSet::new();
        for &num in &ints {
            assert!(unique_check.insert(num));
            assert!(num >= 1 && num <= 10);
        }

        assert_eq!(generate_unique_ints(0, 1, 10), None);
        assert_eq!(generate_unique_ints(5, 10, 1), None);
        assert_eq!(generate_unique_ints(11, 1, 10), None);
    }

    #[test]
    fn test_generate_random_string() {
        let result = generate_random_string(10);
        assert_eq!(result.len(), 10);
        assert!(result.chars().all(|c| c.is_alphanumeric()));
        assert_eq!(generate_random_string(0), "");
        assert_eq!(generate_random_string(MAX_RANDOM_STRING_LENGTH + 1), "");
    }

    #[test]
    fn test_format_commas() {
        assert_eq!(format_commas(123), "123");
        assert_eq!(format_commas(1234), "1,234");
        assert_eq!(format_commas(12345), "12,345");
        assert_eq!(format_commas(123456), "123,456");
        assert_eq!(format_commas(1234567), "1,234,567");
        assert_eq!(format_commas(-1234), "-1,234");
        assert_eq!(format_commas(-1234567), "-1,234,567");
        assert_eq!(format_commas(0), "0");
    }

    #[test]
    fn test_validate_string() {
        assert!(validate_string("valid", 10));
        assert!(!validate_string("", 10));
        assert!(!validate_string("too_long_string", 5));
    }
}
