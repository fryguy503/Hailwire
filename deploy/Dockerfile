FROM node:24-bookworm-slim

WORKDIR /app
COPY *.mjs package.json ./
RUN install -d -o node -g node /var/lib/hailwire
USER node

ENV HAILWIRE_DATA_DIR=/var/lib/hailwire
ENV HAILWIRE_BIND=0.0.0.0
ENV HAILWIRE_PORT=8787

EXPOSE 8787
CMD ["node", "start.mjs"]
