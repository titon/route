<?hh // strict
/**
 * @copyright   2010-2015, The Titon Project
 * @license     http://opensource.org/licenses/bsd-license.php
 * @link        http://titon.io
 */

namespace Titon\Route;

use Titon\Annotation\Reader;
use Titon\Cache\Item;
use Titon\Cache\Storage;
use Titon\Event\EmitsEvents;
use Titon\Event\Event;
use Titon\Event\Subject;
use Titon\Route\Annotation\Route as RouteAnnotation;
use Titon\Route\Event\MatchedEvent;
use Titon\Route\Event\MatchingEvent;
use Titon\Route\Exception\InvalidRouteActionException;
use Titon\Route\Exception\MissingFilterException;
use Titon\Route\Exception\MissingRouteException;
use Titon\Route\Exception\NoMatchException;
use Titon\Route\Matcher\LoopMatcher;
use Titon\Route\Mixin\MethodList;
use Titon\Route\Group as RouteGroup; // Will fatal without alias
use Titon\Utility\Registry;

/**
 * The Router is tasked with the management of routes and matching of routes.
 *
 * @package Titon\Route
 */
class Router implements Subject {
    use EmitsEvents;

    /**
     * Have routes been loaded in from the cache?
     *
     * @var bool
     */
    protected bool $cached = false;

    /**
     * The matched route object.
     *
     * @var \Titon\Route\Route
     */
    protected ?Route $current;

    /**
     * List of filters to trigger for specific routes during a match.
     *
     * @var \Titon\Route\FilterMap
     */
    protected FilterMap $filters = Map {};

    /**
     * List of currently open groups (and their options) in the stack.
     *
     * @var \Titon\Route\GroupList
     */
    protected GroupList $groups = Vector {};

    /**
     * The class to use for route matching.
     *
     * @var \Titon\Route\Matcher
     */
    protected Matcher $matcher;

    /**
     * Mapping of CRUD actions to URL path parts for REST resources.
     * These mappings will be used when creating resource() routes.
     *
     * @var \Titon\Route\ResourceMap {
     *      @var string $list      - GET /resource - List resources
     *      @var string $create    - POST /resource - Create resource
     *      @var string $read      - GET /resource/{id} - Read resource
     *      @var string $update    - PUT /resource/{id} - Update resource
     *      @var string $delete    - DELETE /resource/{id} - Delete resource
     * }
     */
    protected ResourceMap $resourceMap = Map {
        'list' => 'index',
        'create' => 'create',
        'read' => 'read',
        'update' => 'update',
        'delete' => 'delete'
    };

    /**
     * Manually defined aesthetic routes that re-route internally.
     *
     * @var \Titon\Route\RouteMap
     */
    protected RouteMap $routes = Map {};

    /**
     * Storage engine instance.
     *
     * @var \Titon\Cache\Storage
     */
    protected ?Storage $storage;

    /**
     * Initialize the router and prepare for matching.
     */
    public function __construct() {
        $this->matcher = new LoopMatcher();

        // Set events
        $this->on('route.matching', inst_meth($this, 'doLoadRoutes'), 1);
        $this->on('route.matched', inst_meth($this, 'doCacheRoutes'), 1);
        $this->on('route.matched', inst_meth($this, 'doRunFilters'), 2);
    }

    /**
     * Return an action shape into its combined @ formatted representation.
     *
     * @param \Titon\Route\Action $action
     * @return string
     */
    public static function buildAction(Action $action): string {
        return sprintf('%s@%s', $action['class'], $action['action']);
    }

    /**
     * Return the current matched route object.
     *
     * @return \Titon\Route\Route
     */
    public function current(): ?Route {
        return $this->current;
    }

    /**
     * Map a route that only responds to a DELETE request.
     *
     * @param string $key
     * @param \Titon\Route\Route $route
     * @return \Titon\Route\Route
     */
    public function delete(string $key, Route $route): Route {
        return $this->http($key, Vector {'delete'}, $route);
    }

    /**
     * Cache the currently mapped routes.
     * This method is automatically called during the `matched` event.
     *
     * @param \Titon\Event\Event $event
     * @return mixed
     */
    public function doCacheRoutes(Event $event): mixed {
        invariant($event instanceof MatchedEvent, 'Must be a MatchedEvent.');

        $router = $event->getRouter();

        if ($router->isCached()) {
            return true;
        }

        if (($storage = $router->getStorage()) && ($routes = $router->getRoutes())) {
            // Before caching, make sure all routes are compiled
            foreach ($routes as $route) {
                $route->compile();
            }

            // Compiling before hand should speed up the next request
            $storage->save(new Item('routes', serialize($routes), '+1 year'));
        }

        return true;
    }

    /**
     * Loop through and execute for every filter defined in the matched route.
     *
     * @param \Titon\Event\Event $event
     * @return mixed
     */
    public function doRunFilters(Event $event): mixed {
        invariant($event instanceof MatchedEvent, 'Must be a MatchedEvent.');

        $router = $event->getRouter();
        $route = $event->getRoute();

        foreach ($route->getFilters() as $filter) {
            $callback = $this->getFilter($filter);
            $callback($router, $route);
        }

        return true;
    }

    /**
     * Load routes from the cache if they exist.
     * This method is automatically called during the `matching` event.
     *
     * @param \Titon\Event\Event $event
     * @return mixed
     */
    public function doLoadRoutes(Event $event): mixed {
        invariant($event instanceof MatchingEvent, 'Must be a MatchingEvent.');

        $item = $event->getRouter()->getStorage()?->getItem('routes');

        if ($item !== null && $item->isHit()) {
            $this->routes = unserialize($item->get());
            $this->cached = true;
        }

        return true;
    }

    /**
     * Map a filter object to be triggered when a route is matched.
     *
     * @param string $key
     * @param \Titon\Route\Filter $callback
     * @return $this
     */
    public function filter(string $key, Filter $callback): this {
        $this->filters[$key] = inst_meth($callback, 'filter');

        return $this;
    }

    /**
     * Map a filter callback to be triggered when a route is matched.
     *
     * @param string $key
     * @param \Titon\Route\FilterCallback $callback
     * @return $this
     */
    public function filterCallback(string $key, FilterCallback $callback): this {
        $this->filters[$key] = $callback;

        return $this;
    }

    /**
     * Map a route that only responds to a GET request.
     *
     * @param string $key
     * @param \Titon\Route\Route $route
     * @return \Titon\Route\Route
     */
    public function get(string $key, Route $route): Route {
        return $this->http($key, Vector {'get'}, $route);
    }

    /**
     * Return a filter by key.
     *
     * @param string $key
     * @return \Titon\Route\FilterCallback
     * @throws \Titon\Route\Exception\MissingFilterException
     */
    public function getFilter(string $key): FilterCallback {
        if ($this->filters->contains($key)) {
            return $this->filters[$key];
        }

        throw new MissingFilterException(sprintf('Filter %s does not exist', $key));
    }

    /**
     * Return all filters.
     *
     * @return \Titon\Route\FilterMap
     */
    public function getFilters(): FilterMap {
        return $this->filters;
    }

    /**
     * Return the list of currently active groups.
     *
     * @return \Titon\Route\GroupList
     */
    public function getGroups(): GroupList {
        return $this->groups;
    }

    /**
     * Return the matcher object.
     *
     * @return \Titon\Route\Matcher
     */
    public function getMatcher(): Matcher {
        return $this->matcher;
    }

    /**
     * Return the CRUD action resource map.
     *
     * @return \Titon\Route\ResourceMap
     */
    public function getResourceMap(): ResourceMap {
        return $this->resourceMap;
    }

    /**
     * Return a route by key.
     *
     * @param string $key
     * @return \Titon\Route\Route
     * @throws \Titon\Route\Exception\MissingRouteException
     */
    public function getRoute(string $key): Route {
        if ($this->routes->contains($key)) {
            return $this->routes[$key];
        }

        throw new MissingRouteException(sprintf('Route %s does not exist', $key));
    }

    /**
     * Return all routes.
     *
     * @return \Titon\Route\RouteMap
     */
    public function getRoutes(): RouteMap {
        return $this->routes;
    }

    /**
     * Get the storage engine.
     *
     * @return \Titon\Cache\Storage
     */
    public function getStorage(): ?Storage {
        return $this->storage;
    }

    /**
     * Group multiple route mappings into a single collection and apply options to all of them.
     * Can apply path prefixes, suffixes, patterns, filters, methods, conditions, and more.
     *
     * @param \Titon\Route\GroupCallback $callback
     * @return $this
     */
    public function group(GroupCallback $callback): this {
        $group = new Group();

        $this->groups[] = $group;

        $callback($this, $group);

        $this->groups->pop();

        return $this;
    }

    /**
     * Map a route that only responds to a HEAD request.
     *
     * @param string $key
     * @param \Titon\Route\Route $route
     * @return \Titon\Route\Route
     */
    public function head(string $key, Route $route): Route {
        return $this->http($key, Vector {'head'}, $route);
    }

    /**
     * Map a route that only responds to a defined list of HTTP methods.
     *
     * @param string $key
     * @param \Titon\Route\Mixin\MethodList $methods
     * @param \Titon\Route\Route $route
     * @return \Titon\Route\Route
     */
    public function http(string $key, MethodList $methods, Route $route): Route {
        return $this->map($key, $route->setMethods($methods));
    }

    /**
     * Return true if routes have been loaded from a cache.
     *
     * @return bool
     */
    public function isCached(): bool {
        return $this->cached;
    }

    /**
     * Add a custom defined route object that matches to an internal destination.
     *
     * @param string $key
     * @param \Titon\Route\Route $route
     * @return \Titon\Route\Route
     */
    public function map(string $key, Route $route): Route {
        $this->routes[$key] = $route;

        // Apply group options
        foreach ($this->getGroups() as $group) {
            $route->setSecure($group->getSecure());

            if ($prefix = $group->getPrefix()) {
                $route->prepend($prefix);
            }

            if ($suffix = $group->getSuffix()) {
                $route->append($suffix);
            }

            if ($patterns = $group->getPatterns()) {
                $route->addPatterns($patterns);
            }

            if ($filters = $group->getFilters()) {
                $route->addFilters($filters);
            }

            if ($methods = $group->getMethods()) {
                $route->addMethods($methods);
            }

            if ($conditions = $group->getConditions()) {
                $route->addConditions($conditions);
            }
        }

        return $route;
    }

    /**
     * Attempt to match an internal route.
     *
     * @param string $url
     * @return \Titon\Route\Route
     * @throws \Titon\Route\Exception\NoMatchException
     */
    public function match(string $url): Route {
        $this->emit(new MatchingEvent($this, $url));

        $match = $this->getMatcher()->match($url, $this->getRoutes());

        if (!$match) {
            throw new NoMatchException(sprintf('No route has been matched for %s', $url));
        }

        $this->current = $match;

        $this->emit(new MatchedEvent($this, $match));

        return $match;
    }

    /**
     * Map a route that only responds to an OPTIONS request.
     *
     * @param string $key
     * @param \Titon\Route\Route $route
     * @return \Titon\Route\Route
     */
    public function options(string $key, Route $route): Route {
        return $this->http($key, Vector {'options'}, $route);
    }

    /**
     * Parse a URL and apply default routes. Attempt to deconstruct @ URLs.
     *
     * @param string $action
     * @return \Titon\Route\Action
     * @throws \Titon\Route\Exception\InvalidRouteActionException
     */
    public static function parseAction(string $action): Action {
        $matches = [];

        if (preg_match('/^([\w\\\]+)\@(\w+)?$/i', $action, $matches)) {
            return shape(
                'class' => $matches[1],
                'action' => $matches[2]
            );
        }

        throw new InvalidRouteActionException(sprintf('Invalid @ action routing format for %s', $action));
    }

    /**
     * Map a route that only responds to a POST request.
     *
     * @param string $key
     * @param \Titon\Route\Route $route
     * @return \Titon\Route\Route
     */
    public function post(string $key, Route $route): Route {
        return $this->http($key, Vector {'post'}, $route);
    }

    /**
     * Map a route for a GET request, and another for a POST request.
     * This supports the POST-REDIRECT-GET (PRG) pattern.
     *
     * @param string $key
     * @param \Titon\Route\Route $route
     * @return $this
     */
    public function prg(string $key, Route $route): this {
        $action = $route->getAction();
        $actionSuffix = ucfirst($action['action']);

        // Set GET route
        $action['action'] = 'get' . $actionSuffix;

        $get = clone $route;
        $get->setAction($action);

        $this->get($key . '.get', $get);

        // Set POST route
        $action['action'] = 'post' . $actionSuffix;

        $post = clone $route;
        $post->setAction($action);

        $this->post($key . '.post', $post);

        return $this;
    }

    /**
     * Map a route that only responds to a DELETE request.
     *
     * @param string $key
     * @param \Titon\Route\Route $route
     * @return \Titon\Route\Route
     */
    public function put(string $key, Route $route): Route {
        return $this->http($key, Vector {'put'}, $route);
    }

    /**
     * Map multiple routes that point to REST styled resources.
     *
     * @param string $key
     * @param \Titon\Route\Route $route
     * @return $this
     */
    public function resource(string $key, Route $route): this {
        $action = $route->getAction();
        $actionMap = $this->getResourceMap();
        $path = $route->getPath();
        $class = get_class($route);
        $resources = Map {
            'list' => Vector {'get'},
            'create' => Vector {'post'},
            'read' => Vector {'get'},
            'update' => Vector {'put', 'post'},
            'delete' => Vector {'delete', 'post'}
        };

        foreach ($resources as $resource => $methods) {

            // Set the action to trigger
            $newAction = $action;
            $newAction['action'] = $actionMap[$resource];

            // Build the new URL path
            $newPath = rtrim($path, '/');

            if (in_array($resource, Vector {'read', 'update', 'delete'})) {
                $newPath .= '/{id}';
            }

            /** @var \Titon\Route\Route $newRoute */
            $newRoute = Registry::factory($class, [$newPath, static::buildAction($newAction)], false);

            invariant($newRoute instanceof Route, 'Must be a Route');

            $newRoute->setStatic($route->getStatic());
            $newRoute->setSecure($route->getSecure());
            $newRoute->setFilters($route->getFilters());
            $newRoute->setPatterns($route->getPatterns());
            $newRoute->setMethods($methods);

            $this->map($key . '.' . $resource, $newRoute);
        }

        return $this;
    }

    /**
     * Set the matcher.
     *
     * @param \Titon\Route\Matcher $matcher
     * @return $this
     */
    public function setMatcher(Matcher $matcher): this {
        $this->matcher = $matcher;

        return $this;
    }

    /**
     * Update the resource mapping.
     *
     * @param \Titon\Route\ResourceMap $map
     * @return $this
     */
    public function setResourceMap(ResourceMap $map): this {
        $this->resourceMap->setAll($map);

        return $this;
    }

    /**
     * Set the storage engine.
     *
     * @param \Titon\Cache\Storage $storage
     * @return $this
     */
    public function setStorage(Storage $storage): this {
        $this->storage = $storage;

        return $this;
    }

    /**
     * Map routes through annotations on a specified class. If an annotation is found on the class,
     * map as a resource. If an annotation is found on a method, map normally.
     *
     * @param string $class
     * @return $this
     */
    public function wire(string $class): this {
        $reader = new Reader($class);

        // Map resource routes if the annotation is on the class
        foreach ($reader->getClassAnnotations() as $annotation) {
            if ($annotation instanceof RouteAnnotation) {
                $this->resource($annotation->getKey(), $annotation->toRoute($class));
            }
        }

        // Map regular routes if the annotation is on a method
        foreach ($reader->getAnnotatedMethods() as $method => $annotations) {
            foreach ($annotations as $annotation) {
                if ($annotation instanceof RouteAnnotation) {
                    $this->map($annotation->getKey(), $annotation->toRoute($class, $method));
                }
            }
        }

        return $this;
    }

}
