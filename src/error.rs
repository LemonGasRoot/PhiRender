use thiserror::Error;

pub type Result<T> = std::result::Result<T, Error>;

#[derive(Debug, Error)]
pub enum Error {
    #[error("invalid JSON: {0}")]
    Json(#[from] serde_json::Error),
    #[error("unsupported chart: {0}")]
    Unsupported(String),
    #[error("invalid chart data at {path}: {message}")]
    Invalid { path: String, message: String },
    #[error("archive error: {0}")]
    Archive(String),
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
}

pub(crate) fn invalid(path: impl Into<String>, message: impl Into<String>) -> Error {
    Error::Invalid {
        path: path.into(),
        message: message.into(),
    }
}
