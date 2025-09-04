use std::time::Duration;

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
}
