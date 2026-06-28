//! Minimal WebTransport-over-HTTP/3 echo server.
//!
//! - Listens on UDP 127.0.0.1:4433 (hardcoded).
//! - Uses a self-signed certificate; prints its SHA-256 (DER) hash as hex.
//! - Accepts WebTransport sessions on any `:path`.
//! - Echoes datagrams, unidirectional streams, and bidirectional streams.

use std::net::SocketAddr;

use tokio::io::AsyncReadExt;
use wtransport::endpoint::IncomingSession;
use wtransport::tls::Sha256DigestFmt;
use wtransport::{Endpoint, Identity, ServerConfig};

const BIND_ADDR: &str = "127.0.0.1:4433";

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let bind: SocketAddr = BIND_ADDR.parse()?;

    // Self-signed certificate valid for localhost / 127.0.0.1.
    let identity = Identity::self_signed(["localhost", "127.0.0.1"])?;

    // Compute the SHA-256 hash of the DER-encoded end-entity certificate.
    // This is what a native WebTransport client pins via `serverCertificateHashes`.
    let cert = &identity.certificate_chain().as_slice()[0];
    let digest = cert.hash();
    // Plain (un-delimited) lowercase hex.
    let hex: String = digest.as_ref().iter().map(|b| format!("{b:02x}")).collect();

    let config = ServerConfig::builder()
        .with_bind_address(bind)
        .with_identity(identity)
        .build();

    let server = Endpoint::server(config)?;

    println!("WT ECHO SERVER LISTENING on {BIND_ADDR}");
    println!("CERT SHA-256 (hex):    {hex}");
    println!(
        "CERT SHA-256 (dotted): {}",
        digest.fmt(Sha256DigestFmt::DottedHex)
    );

    let mut session_id: u64 = 0;
    loop {
        let incoming = server.accept().await;
        session_id += 1;
        let id = session_id;
        // Handle each session concurrently; never let one session crash the server.
        tokio::spawn(async move {
            if let Err(e) = handle_session(id, incoming).await {
                println!("[session {id}] closed: {e}");
            }
        });
    }
}

async fn handle_session(
    id: u64,
    incoming: IncomingSession,
) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let session_request = incoming.await?;
    let path = session_request.path().to_string();
    let authority = session_request.authority().to_string();

    // Accept on any path.
    let connection = session_request.accept().await?;
    println!(
        "[session {id}] accepted (authority=\"{authority}\", path=\"{path}\", stable_id={})",
        connection.stable_id()
    );

    let conn = std::sync::Arc::new(connection);

    // Datagram echo loop.
    let dgram_conn = conn.clone();
    let dgram_task = tokio::spawn(async move {
        loop {
            match dgram_conn.receive_datagram().await {
                Ok(dgram) => {
                    let payload = dgram.payload();
                    println!("[session {id}] datagram {} bytes", payload.len());
                    if let Err(e) = dgram_conn.send_datagram(&payload) {
                        println!("[session {id}] datagram send error: {e}");
                    }
                }
                Err(e) => {
                    println!("[session {id}] datagram loop ended: {e}");
                    break;
                }
            }
        }
    });

    // Uni stream echo loop.
    let uni_conn = conn.clone();
    let uni_task = tokio::spawn(async move {
        loop {
            match uni_conn.accept_uni().await {
                Ok(mut recv) => {
                    let conn = uni_conn.clone();
                    tokio::spawn(async move {
                        let mut buf = Vec::new();
                        if let Err(e) = recv.read_to_end(&mut buf).await {
                            println!("[session {id}] uni read error: {e}");
                            return;
                        }
                        println!("[session {id}] uni stream {} bytes", buf.len());
                        match conn.open_uni().await {
                            Ok(opening) => match opening.await {
                                Ok(mut send) => {
                                    if let Err(e) = send.write_all(&buf).await {
                                        println!("[session {id}] uni write error: {e}");
                                        return;
                                    }
                                    if let Err(e) = send.finish().await {
                                        println!("[session {id}] uni finish error: {e}");
                                    }
                                    println!("[session {id}] uni echo sent {} bytes back", buf.len());
                                }
                                Err(e) => println!("[session {id}] uni open(await) error: {e}"),
                            },
                            Err(e) => println!("[session {id}] uni open error: {e}"),
                        }
                    });
                }
                Err(e) => {
                    println!("[session {id}] uni loop ended: {e}");
                    break;
                }
            }
        }
    });

    // Bidi stream echo loop.
    let bi_conn = conn.clone();
    let bi_task = tokio::spawn(async move {
        loop {
            match bi_conn.accept_bi().await {
                Ok((mut send, mut recv)) => {
                    tokio::spawn(async move {
                        // Echo chunk-by-chunk so we stream as the client sends,
                        // finishing when the client finishes its half.
                        let mut buf = vec![0u8; 64 * 1024];
                        let mut total = 0usize;
                        loop {
                            match recv.read(&mut buf).await {
                                Ok(Some(n)) => {
                                    total += n;
                                    if let Err(e) = send.write_all(&buf[..n]).await {
                                        println!("[session {id}] bidi write error: {e}");
                                        return;
                                    }
                                }
                                Ok(None) => break, // client finished
                                Err(e) => {
                                    println!("[session {id}] bidi read error: {e}");
                                    return;
                                }
                            }
                        }
                        println!("[session {id}] bidi stream {total} bytes");
                        if let Err(e) = send.finish().await {
                            println!("[session {id}] bidi finish error: {e}");
                        }
                    });
                }
                Err(e) => {
                    println!("[session {id}] bidi loop ended: {e}");
                    break;
                }
            }
        }
    });

    // Wait until the connection is fully closed.
    let reason = conn.closed().await;
    println!("[session {id}] connection closed: {reason}");

    dgram_task.abort();
    uni_task.abort();
    bi_task.abort();

    Ok(())
}
