use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Debug, Deserialize, Serialize, Clone)]
pub struct Album {
    pub album_id: u64,
    pub path: String,
}

#[derive(Debug, Deserialize, Serialize, Clone)]
pub struct Track {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub id: Option<String>,

    pub track_id: u64,
    pub track_name: String,
    pub album: Album,
    pub cids: Vec<String>,

    #[serde(skip_serializing_if = "Option::is_none")]
    pub byte_range: Option<[u64; 2]>,

    #[serde(skip_serializing_if = "Option::is_none")]
    pub gdr_account_id: Option<i32>,

    #[serde(skip_serializing_if = "Option::is_none")]
    pub email: Option<String>,
}

impl Track {
    pub fn new(
        id: Option<String>,
        track_id: u64,
        track_name: impl Into<String>,
        album_id: u64,
        path: impl Into<String>,
        byte_range: Option<[u64; 2]>,
        gdr_account_id: Option<i32>,
        email: Option<String>,
    ) -> Self {
        Self {
            id,
            track_id,
            track_name: track_name.into(),
            album: Album {
                album_id,
                path: path.into(),
            },
            cids: Vec::new(),
            byte_range,
            gdr_account_id,
            email,
        }
    }

    pub fn to_value(&self) -> Value {
        serde_json::to_value(self).unwrap()
    }
}
